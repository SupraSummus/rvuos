#!/usr/bin/env python3
"""Read a trace log of user/wifi/esp32c6/: the snapshot list, the compare of two runs against the traced one, and
the library and function each device access belongs to.

  snapshot LOG              the aligned whole-word addresses the run read, as the snap= lines a later run reads back
  compare DRY... TRACED     the traced run's final state against the dry runs', nonzero on a divergence
  diff BASE... OWN          every place the own run differs, the bases naming the volatile words
  attrib [--only NAME] [--segments] LOG  the library and function each access is in, see below
  replay --function NAME... LOG          the named functions' own accesses, which the host replays the own code on

The root task of root.c owns what this parses, see its trace_line and trace_dump. A log is meant to be whole: the
writer no longer loses the odd line, so a short log is a fault to report, and each subcommand says what it found of
one, and wrongs its answer only where the loss lies beside what it reports. `attrib --segments` refuses a short log,
whose loss would shift every segment past it.

Usage: tools/mac-trace.py {snapshot,compare,diff,attrib,replay} ... ; each subcommand has its own --help.
"""

import argparse
import bisect
import collections
import difflib
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
RECORD = re.compile(r"^\s*[\d.]+\s+\[\d+\]\s+pc\s+(0x[0-9a-fA-F]+)\s+t(\d+)\s+([WR])(\d)\s+(0x[0-9a-fA-F]+)"
                    r"\s*=\s*(0x[0-9a-fA-F]+)")
# A cycle's header: "[seq] cycle xT of L:", the L records after it printed once for T turns; see root.c's trace_out_cycle.
CYCLE = re.compile(r"\[\d+\]\s+cycle x(\d+) of (\d+):")
# The stream's own count of the accesses it served: "[seq] trace over: N accesses".
OVER = re.compile(r"trace over:\s*(\d+)\s+accesses")
# A line that carries a sequence number but is no record: the stream's own markers, see root.c's trace_dump.
MARKER = re.compile(r"\[\d+\]\s+(?:trace over:|snapshot of\b)")
# The sequence number at the front of a stream line, whichever it is.
SEQ = re.compile(r"\[(\d+)\]")
# A driver step or an adapter call in trace=1: "trace: req N tX name a0 a1"; see osi.c's osi_trace. It prints them
# from the driver's child log, so their lines reach the file delayed, out of request order, and one can be cut.
TRACE_REQ = re.compile(r"trace:\s*req\s+(\d+)\s+t(\d+)\s+(\S+)\s+(\S+)\s+(\S+)")
# A thread's role, the dump's line before the entries: "trace: thread N role"; see osi.c's osi_trace_dump.
ROLE = re.compile(r"trace: thread (\d+) (\S+)")
# A snapshot line: "snap 0xaddr = 0xvalue", and its block's header, "snapshot of N addresses"; see root.c's trace_dump.
SNAP = re.compile(r"snap (0x[0-9a-fA-F]+) = (0x[0-9a-fA-F]+)")
SNAPHDR = re.compile(r"snapshot of (\d+) addresses")
# The line tools/esp32c6-run.py writes before it boots, naming what it flashed and its hash.
FLASHED = re.compile(r"# flashed 0x[0-9a-fA-F]+ sha256 ([0-9a-f]{64})")
# A ROM linker script: "name = 0x...", possibly inside PROVIDE(...), comments left to strip.
PLACED = re.compile(r"^\s*(?:PROVIDE\s*\(\s*)?([A-Za-z_][A-Za-z0-9_.$]*)\s*=\s*0x[0-9a-fA-F]+", re.M)
# The words whose value is the moment's, not the code's, each with its reason: a count of events over the window,
# which the traced window, taking longer, makes larger, or a measurement such as the chip's temperature.
# `compare` reads them apart from the verdict. A word is here for its reason -- never because two runs disagreed on it.
COUNTERS = {
    0x600A708C: "the frames the air carries over the window, more of them while the window is traced",
    0x6000E058: "the temperature sensor's reading, TSENS_OUT in bits 0 to 7, which the chip's warmth moves",
}


def scan(path):
    """The accesses a log holds, and where it is not whole.

    Returns (records, damaged). A record is (seq, pc, op, width, address, value, turns, thread): turns is what a
    cycle printed it once for, one outside a cycle. A damaged entry is (seq, why) for a line with a sequence number
    that is no record, and for a sequence number missing from the run. A damaged line's own access is lost with it.
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
                out.append((seq, int(r.group(1), 16), r.group(3), int(r.group(4)), int(r.group(5), 16),
                            int(r.group(6), 16), turns, int(r.group(2))))
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


def flashed_from(path, image):
    """Refuse a log that did not flash this file: its names and its objects' addresses are another build's."""
    want = hashlib.sha256(open(image, "rb").read()).hexdigest()
    with open(path, errors="replace") as f:
        if want not in {m.group(1) for m in FLASHED.finditer(f.read())}:
            sys.exit("mac-trace: %s did not flash %s" % (path, image))


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
    for seq, pc, op, width, address, _value, turns, _thread in acc:
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
    """The words the dry runs differ in, then the traced run's divergences from them; the count of those.

    A word any pair of the dry runs differs in is volatile, so that a word which varies run to run but which two of
    them happen to agree on is still set aside; two dry runs are the least. The words COUNTERS names are read apart
    from the verdict, since their count depends on how long the window takes; see its reason.
    """
    dry = [snapshots(p) for p in args.dry]
    traced = snapshots(args.traced)
    for d in dry:
        if set(d) != set(dry[0]) or set(d) != set(traced):
            sys.exit("mac-trace: the runs' snapshots name different addresses")
    volatile = sorted({a for i in range(len(dry)) for j in range(i + 1, len(dry)) for a in dry[i]
                       if dry[i][a] != dry[j][a]})
    counters = sorted(a for a in dry[0]
                      if a not in volatile and a in COUNTERS and traced[a] != dry[0][a])
    divergent = sorted(a for a in dry[0]
                       if a not in volatile and a not in COUNTERS and traced[a] != dry[0][a])
    print("mac-trace: %d addresses; %d volatile between the dry runs; %d counters, not compared; %d divergences"
          % (len(dry[0]), len(volatile), len(counters), len(divergent)))
    for a in volatile:
        print("  volatile  0x%08x  dry %s" % (a, " / ".join("0x%08x" % d[a] for d in dry)))
    for a in counters:
        print("  counter   0x%08x  dry 0x%08x / traced 0x%08x  (%s)" % (a, dry[0][a], traced[a], COUNTERS[a]))
    for a in divergent:
        print("  divergent 0x%08x  dry 0x%08x / traced 0x%08x" % (a, dry[0][a], traced[a]))
    return 1 if divergent else 0


def by_thread(acc, lo, hi, library, fold, join=False, apart=frozenset()):
    """The accesses in (lo, hi], split by thread, each unbroken run of a folded library turned into one marker.

    A record a cycle printed once stands for `turns` accesses, and one crossing the range's edge keeps its share,
    as `attrib`'s segments do. Each thread's accesses are kept apart, since another thread's (the interrupt's)
    interleave asynchronously; a run of a library named in `fold` (libphy's calibration, whose count and values are
    the time's) becomes a single ("*", name, ...) marker. With `join`, every thread's accesses but those `apart`
    names go into one stream, for a run whose own start makes the bring-up on the thread that called it where the
    libraries posted it to another. Returns {thread: [element, ...]}, an element being (op, width, address, value,
    turns) or a marker.
    `library` is None when nothing folds.
    """
    out = collections.defaultdict(list)
    cum = 0
    for _seq, pc, op, width, address, value, turns, thread in acc:
        first, last = cum + 1, cum + turns
        cum = last
        a, b = max(first, lo + 1), min(last, hi)
        if a > b:
            continue
        seq = out[0 if join and thread not in apart else thread]
        if fold:
            lib = library(pc)
            if lib in fold:
                if not seq or seq[-1][0] != "*" or seq[-1][1] != lib:
                    seq.append(("*", lib, 0, 0, 0))
                continue
        seq.append((op, width, address, value, b - a + 1))
    return out


def phase(path, name):
    """The request number of the first trace: line named `name`, or None if the log carries none."""
    for req, _thread, what in trace_landmarks(path):
        if what == name:
            return req
    return None


def align(base, own, volatile):
    """Where two record lists differ, aligned, and the polling notes in what they share.

    Two records agree when their operation, width and address agree, and their values agree unless the address is
    volatile; the turns a cycle printed a record for are left out, so a polling loop that turned a different number
    of times (its count is the time's) aligns, and is a note. The lists are aligned as a whole, so a difference does
    not hide what comes after it: a step that writes one word its own way is one block, and the rest is still held.
    Returns (blocks, notes): a block is (base from, base to, own from, own to), half-open; a note is (base, own).
    """
    def key(e):
        return e[:2] if e[0] == "*" else (e[0], e[1], e[2], None if e[2] in volatile else e[3])

    matcher = difflib.SequenceMatcher(None, [key(e) for e in base], [key(e) for e in own], autojunk=False)
    blocks, notes = [], []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag != "equal":
            blocks.append((i1, i2, j1, j2))
        else:
            notes += [(i1 + k, j1 + k) for k in range(i2 - i1) if base[i1 + k][4] != own[j1 + k][4]]
    return blocks, notes


# The records of a differing block shown on each side; the rest are counted.
SHOWN = 8


def span(lo, hi):
    """A half-open range of records, counted from one, as a diff line names it."""
    if hi == lo:
        return "nothing, before record %d" % (lo + 1)
    return "record %d" % (lo + 1) if hi == lo + 1 else "records %d-%d" % (lo + 1, hi)


def element(e):
    """A record or a folded marker, as a diff line reads it."""
    if e[0] == "*":
        return "(%s)" % e[1]
    return "%c%d 0x%08x = 0x%08x x%d" % (e[0], e[1], e[2], e[3], e[4])


def diff(args):
    """Every place the own run differs, thread by thread, the bases naming the volatile words.

    The base runs are of the same image, so a word any pair of them reads differently is volatile and its value is
    set aside; two bases are the least, and more are given when a word that varies run to run agrees between two by
    chance. The own run is held to the first base. Each thread is compared on its own, since the interrupt's accesses
    interleave asynchronously; a thread whose every access is an interrupt handler's is compared as a multiset and
    reported apart, not as a failure. The range comes from the trace: lines named by --from and --to (the whole
    window by default), resolved in each log, so a run whose own start adds or drops an access is aligned by its
    phase markers, not by request number. A library named by --collapse folds into one marker per run (libphy's
    calibration, whose count and values are the time's). With --join every thread is one stream, for a run whose own
    start makes the bring-up on the thread that called it where the libraries posted it to another, the interrupt's
    and the timers' kept apart all the same. A polling loop
    whose cycle turned a different number of times is a note, never a difference. The streams are aligned as a
    whole, so every block where they differ is shown, not the first alone. A short log is refused,
    and with --image a log that did not flash that file, since the names come from the ELF of the build at hand.
    """
    paths = args.base + [args.own_log]
    if args.image:
        for path in paths:
            flashed_from(path, args.image)
    logs = [scan(p) for p in paths]
    for path, (_acc, bad) in zip(paths, logs):
        if bad:
            sys.exit("mac-trace: %s is short in %d places" % (path, len(bad)))
    fold = set(args.collapse)
    library, _function = classifier(args, {r[1] for acc, _bad in logs for r in acc})

    def bounds(path):
        lo = phase(path, args.from_) if args.from_ else 0
        hi = phase(path, args.to) if args.to else None
        if (args.from_ and lo is None) or (args.to and hi is None):
            sys.exit("mac-trace: %s has no trace: line named %s" % (path, args.from_ if lo is None else args.to))
        return lo, hi if hi is not None else 1 << 62

    # A thread the driver's role lines name as the interrupt's or a timer's has its accesses placed by the
    # hardware's timing, so its stream is compared as a multiset and apart, not positionally, --join or not.
    async_t = set()
    for path in paths:
        async_t |= {t for t, role in roles(path).items() if role in ("isr", "timers")}

    def take(acc, path):
        lo, hi = bounds(path)
        return by_thread(acc, lo, hi, library, fold, args.join, async_t)

    bases = [take(logs[i][0], paths[i]) for i in range(len(args.base))]
    own = take(logs[-1][0], args.own_log)

    volatile = set()
    for t in sorted({t for b in bases for t in b}):
        seqs = [b.get(t, []) for b in bases]
        for i in range(len(seqs)):
            for j in range(i + 1, len(seqs)):
                a, c = seqs[i], seqs[j]
                for k in range(min(len(a), len(c))):
                    if a[k][:3] != c[k][:3]:
                        print("mac-trace: thread %d: two bases differ in shape at record %d" % (t, k + 1))
                        break
                    if a[k][3] != c[k][3]:
                        volatile.add(a[k][2])

    bad = 0
    for t in sorted({t for b in bases for t in b} | set(own)):
        a, o = bases[0].get(t, []), own.get(t, [])
        if t in async_t:
            extra = collections.Counter(a) - collections.Counter(o)
            missing = collections.Counter(o) - collections.Counter(a)
            if extra or missing:
                print("mac-trace: thread %d is an interrupt's, whose accesses the hardware's timing places:" % t)
                for e, n in extra.items():
                    print("  base has x%d  %s" % (n, element(e)))
                for e, n in missing.items():
                    print("  own has x%d  %s" % (n, element(e)))
            continue
        blocks, notes = align(a, o, volatile)
        for i, j in notes:
            print("  t%d polling %5d  0x%08x  x%d vs x%d" % (t, i + 1, a[i][2], a[i][4], o[j][4]))
        if blocks:
            bad = 1
            print("mac-trace: thread %d: base %d records, own %d; they differ in %d place%s:"
                  % (t, len(a), len(o), len(blocks), "" if len(blocks) == 1 else "s"))
        for i1, i2, j1, j2 in blocks:
            print("  base %s, own %s:" % (span(i1, i2), span(j1, j2)))
            for side, recs, lo, hi in (("base", a, i1, i2), ("own ", o, j1, j2)):
                for k in range(lo, min(hi, lo + SHOWN)):
                    print("    %s %5d  %s" % (side, k + 1, element(recs[k])))
                if hi - lo > SHOWN:
                    print("    %s ... and %d more" % (side, hi - lo - SHOWN))
    if bad:
        return 1
    print("mac-trace: every thread agrees, by operation, width and address, and by value, %d volatile words aside"
          % len(volatile))
    return 0


def trace_landmarks(path):
    """The trace=1 driver steps and adapter calls, (req, thread, name), in request order.

    `req` is the accesses the driver had served when the call was made, so each line bounds the accesses before it
    from those after. The child log delays their lines, so they are sorted by `req` here; ties keep the file's
    order. A line the child log cut is lost, and the window's own end ("done") carries the stream's total.
    """
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = TRACE_REQ.search(line)
            if m:
                out.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    out.sort(key=lambda t: t[0])
    return out


def roles(path):
    """{thread: role} from the dump's "trace: thread N role" lines, which osi.c's osi_trace_dump owns."""
    out = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = ROLE.search(line)
            if m:
                out[int(m.group(1))] = m.group(2)
    return out


def trace_segments(acc, marks, library, function, top):
    """The accesses between the trace: lines, each segment named by the calls that begin it.

    A segment runs from one request number to the next, so the adapter calls frame the start's bring-up the way
    the stop's group table framed the stop. A record a cycle collapsed stands for `turns` equal accesses and is
    split across a boundary by its share. `top` says how many functions to name under a segment; 0 names all.
    """
    groups = []  # (req, [name, ...]): the calls at one request number, in the file's order
    for req, _thread, name in marks:
        if groups and groups[-1][0] == req:
            groups[-1][1].append(name)
        else:
            groups.append((req, [name]))
    spans = []  # (first, last, pc): the accesses a printed record stands for, counted from one
    cum = 0
    for _seq, pc, _op, _width, _address, _value, turns, _thread in acc:
        spans.append((cum + 1, cum + turns, pc))
        cum += turns
    for i, (req, names) in enumerate(groups):
        end = groups[i + 1][0] if i + 1 < len(groups) else cum
        by_lib = collections.Counter()
        by_fun = collections.Counter()
        for first, last, pc in spans:
            a, b = max(first, req + 1), min(last, end)
            if a <= b:
                lib = library(pc)
                by_lib[lib] += b - a + 1
                by_fun[(lib, function.get(pc, "?"))] += b - a + 1
        print("\nreq %d -> %d  %s" % (req, end, ", ".join(names)))
        named = ", ".join("%s %d" % (lib, n) for lib, n in by_lib.most_common())
        print("  %d accesses%s" % (sum(by_lib.values()), ": " + named if named else ""))
        for (lib, fn), n in by_fun.most_common(top or None):
            print("    %-46s %d" % (fn, n))


def classifier(args, pcs):
    """A function pc -> library, and pc -> function name, from the ELF and the --lib/--own archives.

    A library defines its own symbols; the ROM's are the ones a --lib's script places; what neither owns is the
    driver's objects ("own"), the ROM's below the image's base, or "unknown". `attrib` and `diff` share it.
    """
    owner, romlib = {}, {}
    for spec in args.lib:
        name, rest = spec.split("=", 1)
        archive, _, script = rest.partition(":")
        for s in defined(archive):
            owner.setdefault(s, name)
        if script:
            for s in placed(script):
                romlib.setdefault(s, name)
    for pattern in args.own:
        for obj in glob.glob(pattern, recursive=True):
            for s in defined(obj, external=False):
                owner[s] = "own"
    pcs = sorted(pcs)
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

    return library, function


def attrib(args):
    """The library and function each access is in, from the ELF's names and the libraries' symbols.

    It refuses only when the log is short beside the library --only asks for, whose part is then not whole; with
    no --only its counts are lower bounds over the whole log, and every memory-mapped access is named.
    """
    if args.image:
        flashed_from(args.log, args.image)

    acc, damaged = scan(args.log)
    library, function = classifier(args, {r[1] for r in acc})
    names = [spec.split("=", 1)[0] for spec in args.lib]

    by_records = collections.Counter()
    by_accesses = collections.Counter()
    by_function = collections.Counter()
    known = []  # (seq, library), in stream order, to name what a damaged sequence number sits beside
    for seq, pc, op, width, address, _value, turns, _thread in acc:
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

    marks = trace_landmarks(args.log)
    if marks and not damaged and marks[-1][0] > sum(by_accesses.values()):
        sys.exit("mac-trace: a trace: line names req %d, but the stream served %d accesses"
                 % (marks[-1][0], sum(by_accesses.values())))
    if args.segments:
        if damaged:
            sys.exit("mac-trace: %s is short in %d places, so the segments past a loss would be shifted"
                     % (args.log, len(damaged)))
        if not marks:
            print("\nthe trace: lines: none in %s; the window ran without them" % args.log)
        else:
            print("\nthe trace: lines' segments (the calls begin each):")
            trace_segments(acc, marks, library, function, args.top)
        return 0

    shown = [args.only] if args.only else [n for n in names + ["own", "rom", "unknown"] if n in by_records]
    for lib in shown:
        print("\n%s:" % lib)
        for (l, fn), n in by_function.most_common():
            if l == lib:
                print("  %-46s %d" % (fn, n))
        if args.only:
            print("  ordered writes:")
            for seq, pc, op, width, address, _value, turns, _thread in acc:
                if library(pc) == lib and op == "W":
                    print("    %-46s w%d 0x%08x" % (function.get(pc, "?"), width, address))


def replay(args):
    """The accesses the named functions made, in the log's order, as the host's replay of the own code reads them.

    One line each, "R4 0xaddress 0xvalue" or "W4 ...", after a "#" line naming the functions, the bounds and the
    log's image;
    what the functions' callees did is left out, as the own code calls those.
    --from and --to bound them by other functions' first accesses, so that a caller's calls are told from another's.
    A log short anywhere is refused, as is a function no access names, and a cycle, which the replay does not unroll.
    """
    if args.image:
        flashed_from(args.log, args.image)
    acc, damaged = scan(args.log)
    if damaged:
        sys.exit("mac-trace: %s is short in %d places, so a function's accesses may be missing"
                 % (args.log, len(damaged)))
    _, function = classifier(args, {r[1] for r in acc})

    def first(name):
        for i, r in enumerate(acc):
            if function.get(r[1]) == name:
                return i
        sys.exit("mac-trace: no access of %s in %s" % (name, args.log))

    lo = first(args.from_) if args.from_ else 0
    hi = first(args.to) if args.to else len(acc)
    acc = acc[lo:hi]
    wanted = set(args.function)
    mine = [r for r in acc if function.get(r[1]) in wanted]
    missing = wanted - {function[r[1]] for r in mine}
    if missing:
        sys.exit("mac-trace: no access of %s in %s" % (", ".join(sorted(missing)), args.log))
    cycled = [r for r in mine if r[6] > 1]
    if cycled:
        sys.exit("mac-trace: record %d of %s is in a cycle, which the replay does not unroll"
                 % (cycled[0][0], function[cycled[0][1]]))
    sha = None
    with open(args.log, errors="replace") as f:
        for line in f:
            m = FLASHED.search(line)
            if m:
                sha = m.group(1)
    bounds = (" from %s" % args.from_ if args.from_ else "") + (" up to %s" % args.to if args.to else "")
    print("# %s%s, in a run of the image sha256 %s" % (" ".join(args.function), bounds, sha or "unknown"))
    for _seq, _pc, op, width, address, value, _turns, _thread in mine:
        print("%s%d 0x%08x 0x%08x" % (op, width, address, value))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("snapshot", help="a trace log's reads, as the snap= lines a later run reads back")
    p.add_argument("log")
    p.set_defaults(run=snapshot)

    p = sub.add_parser("compare", help="the traced run's final state against the dry runs'")
    p.add_argument("dry", nargs="+", metavar="DRY", help="two or more dry runs; a word any pair differs in is volatile")
    p.add_argument("traced")
    p.set_defaults(run=compare)

    p = sub.add_parser("diff", help="every place the own run differs, the bases naming the volatile words")
    p.add_argument("--from", dest="from_", metavar="NAME", help="start at the request of this trace: line")
    p.add_argument("--to", dest="to", metavar="NAME", help="stop before the request of this trace: line")
    p.add_argument("--join", action="store_true",
                   help="compare every thread as one stream (a run whose own start makes the bring-up on one thread)")
    p.add_argument("--collapse", action="append", default=[], metavar="NAME",
                   help="a library whose runs fold into one marker, repeatable; the libraries below are then needed")
    p.add_argument("--elf", required=True, help="the driver's ELF, for the image's code")
    p.add_argument("--rom", required=True, help="the ROM's ELF, for the ROM's code")
    p.add_argument("--lib", action="append", default=[], metavar="NAME=ARCHIVE[:ROM.ld]",
                   help="a library, by the archive that defines it and the ROM script that places the rest")
    p.add_argument("--own", action="append", default=[], metavar="OBJ",
                   help="the driver's own objects, by path or glob; the owner not any library's")
    p.add_argument("--image", metavar="FILE", help="refuse a log, base or own, unless it flashed this file")
    p.add_argument("base", nargs="+", metavar="BASE", help="two or more runs of the image; a word any pair differs in is volatile")
    p.add_argument("own_log", metavar="OWN", help="the own run's log")
    p.set_defaults(run=diff)

    p = sub.add_parser("attrib", help="the library and function each access is in")
    p.add_argument("--elf", required=True, help="the driver's ELF, for the image's code")
    p.add_argument("--rom", required=True, help="the ROM's ELF, for the ROM's code")
    p.add_argument("--lib", action="append", default=[], metavar="NAME=ARCHIVE[:ROM.ld]",
                   help="a library, by the archive that defines it and the ROM script that places the rest")
    p.add_argument("--own", action="append", default=[], metavar="OBJ",
                   help="the driver's own objects, by path or glob; the owner not any library's")
    p.add_argument("--image", metavar="FILE", help="refuse unless the log flashed this file, by its sha256")
    p.add_argument("--only", metavar="NAME", help="hold this library alone, and refuse only if it is the damaged one")
    p.add_argument("--segments", action="store_true",
                   help="the accesses between the trace: lines, by library and function, instead of the lists")
    p.add_argument("--top", type=int, default=8, metavar="N", help="functions to name under a segment, 0 for all")
    p.add_argument("log")
    p.set_defaults(run=attrib)

    p = sub.add_parser("replay", help="the named functions' accesses, for the host's replay of the own code")
    p.add_argument("--elf", required=True, help="the driver's ELF, for the image's code")
    p.add_argument("--rom", required=True, help="the ROM's ELF, for the ROM's code")
    p.add_argument("--lib", action="append", default=[], metavar="NAME=ARCHIVE[:ROM.ld]",
                   help="a library, by the archive that defines it and the ROM script that places the rest")
    p.add_argument("--own", action="append", default=[], metavar="OBJ",
                   help="the driver's own objects, by path or glob; the owner not any library's")
    p.add_argument("--image", metavar="FILE", help="refuse unless the log flashed this file, by its sha256")
    p.add_argument("--function", action="append", required=True, metavar="NAME",
                   help="a function whose own accesses to take, repeatable; its callees' are left out")
    p.add_argument("--from", dest="from_", metavar="NAME", help="start at this function's first access")
    p.add_argument("--to", metavar="NAME", help="stop before this function's first access")
    p.add_argument("log")
    p.set_defaults(run=replay)

    args = parser.parse_args()
    sys.exit(args.run(args))


if __name__ == "__main__":
    main()
