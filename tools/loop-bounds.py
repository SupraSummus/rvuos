#!/usr/bin/env python3
"""Fail the link on a loop a trap can run whose source does not say what bounds it.

DESIGN.md, "Bounded work", says what may bound a loop, and kernel/work.h how the source says it.

The loops are found in the image, so one from a macro, an inlined function or the compiler counts too.
A loop is a strongly connected component of a function's control flow;
the loops inside it are the components left without the edges back into it.
A jr through a table clang laid out goes to that table's targets;
any other may go to every word of the kernel's data that points into its function.
A call to a function that never returns ends its block,
and a block that leads only to such calls is where the machine stops, so its loops are left out.

Each loop is matched to the innermost source loop around every instruction of its body,
through the instruction's inlined frames and the loop ranges of clang's AST.
So an inlined loop is matched where it came from,
and an unrolled inner loop leaves its outer loop to answer for it.
A loop that matches no source loop, or matches the one a loop around it matches, fails.

A loop bounded by an argument puts the question to each call, made or inlined,
which must follow a call annotation.
A bound that the loop's own shape limits must not be below that limit.
A walk must be a row of the table in "Bounded work", and every row walked or paid for.
The self-check is not checked, and every call to it must stand in an if on debug_trace.

Usage: loop-bounds.py [--objdump tool] [--symbolizer tool] --cc cc --cflags flags
                      --sources "file.c..." --design DESIGN.md kernel.elf file.su...
"""

import argparse
import bisect
import re
import shlex
import struct
import subprocess
import sys

from kimage import Failure, NO_DEST, SHF_ALLOC, TARGET, data_words, imm, load, text_end
from ksource import Source, counted, symbolize

ENTRIES = {"trap_vectors"}
# The self-check walks every object by design and runs only while OP_DEBUG_TRACE is on:
# each cut, and the variable every call to it must be guarded by.
CUTS = {"selfcheck_run": "debug_trace"}
# What a paid loop may take away, as host/work.c counts it.
PAID_UNITS = {"node", "link", "object", "waiter"}

BRANCH = re.compile(r"^b(eq|ne|lt|ge|ltu|geu|eqz|nez|lez|gez|ltz|gtz|gt|le|gtu|leu)$")


def walk_rows(design: str) -> set[str]:
    """The names in the first column of DESIGN.md's table of walks."""
    text = open(design).read()
    start = text.index("**Where the kernel falls short.**")
    rows = set()
    table = False
    for line in text[start:].splitlines()[1:]:
        if not line.startswith("|"):
            if table:
                break
            continue
        table = True
        rows.update(re.findall(r"`(\w+)`", line.split("|")[1]))
    return rows


class Block:
    def __init__(self, start):
        self.start = start
        self.end = start
        self.succ = set()
        self.calls = []  # (pc, callee)
        self.leaves = False  # returns to the caller, itself or by a tail call
        self.tails = []  # (pc, function) of the jumps to functions that return for it
        self.indirect = False  # calls or jumps through a register to another function


TERMINATORS = ("ret", "mret", "j", "jr")


def defined(insns, i, reg):
    """The value an li, or an lui or auipc with its addi, left in reg before insns[i]:
    the nearest such write above it, with nothing else writing reg in between."""
    for j in range(i - 1, -1, -1):
        pc, mnem, ops = insns[j]
        args = [x.strip() for x in ops.split(",")]
        if not args or args[0] != reg or NO_DEST.match(mnem):
            continue
        if mnem == "li":
            return imm(args[1])
        if mnem == "addi" and args[1] == reg and j > 0:
            ppc, pm, pops = insns[j - 1]
            pargs = [x.strip() for x in pops.split(",")]
            if pm in ("lui", "auipc") and pargs[0] == reg:
                high = (imm(pargs[1]) << 12) & 0xFFFFFFFF
                if pm == "auipc":
                    high = (ppc + high) & 0xFFFFFFFF
                return (high + imm(args[2])) & 0xFFFFFFFF
        return None
    return None


def args_of(ops):
    return [x.strip() for x in ops.split(",")] if ops else []


def writes(mnem, ops):
    """The register an instruction writes, or None."""
    args = args_of(ops)
    return None if not args or NO_DEST.match(mnem) else args[0]


def find_back(insns, i, want, keep, reach=8):
    """The nearest j above i, within reach, that want accepts,
    with no instruction between writing a register in keep."""
    for j in range(i - 1, max(i - 1 - reach, -1), -1):
        _, mnem, ops = insns[j]
        if want(mnem, args_of(ops)):
            return j
        if writes(mnem, ops) in keep or mnem in TERMINATORS or BRANCH.match(mnem):
            return None
    return None


def jump_table(image, sections, insns, i):
    """The targets of the jr at insns[i] if it goes through a table clang laid out, else None.

    The shape is a bound check on the index, then
    slli s, idx, 2; add t, s, base; lw r, 0(t); jr r, maybe with other instructions between,
    the base and the bound each from an li, or an lui or auipc pair, above.
    """
    reg = insns[i][2].strip()
    j = find_back(insns, i, lambda m, a: m == "lw" and a[0] == reg, {reg})
    if j is None:
        return None
    t = re.fullmatch(r"0x0\((\w+)\)", args_of(insns[j][2])[1])
    if not t:
        return None
    t = t.group(1)
    k = find_back(insns, j, lambda m, a: m == "add" and a[0] == t, {t})
    if k is None:
        return None
    _, _, add_ops = insns[k]
    sources = args_of(add_ops)[1:]
    l = find_back(insns, k, lambda m, a: m == "slli" and a[0] in sources and a[2] == "0x2", set())
    if l is None:
        return None
    shifted, index = args_of(insns[l][2])[:2]
    if any(writes(mn, o) == shifted for _, mn, o in insns[l + 1:k]):
        return None
    base_reg = sources[1] if sources[0] == shifted else sources[0]
    base = defined(insns, k, base_reg)
    # The bound check ends the block above: the branch just before the table's first instruction.
    m = l - 1
    while m >= 0 and not BRANCH.match(insns[m][1]):
        if writes(insns[m][1], insns[m][2]) == index:
            return None
        m -= 1
    if m < 0:
        return None
    check, check_args = insns[m][1], args_of(insns[m][2])
    if check == "bltu" and check_args[1] == index:
        limit = defined(insns, m, check_args[0])  # bltu limit, idx: out of range above limit
        count = None if limit is None else limit + 1
    elif check == "bgeu" and check_args[0] == index:
        count = defined(insns, m, check_args[1])
    else:
        return None
    if base is None or count is None or not 0 < count <= 4096:
        return None
    for _, _, flags, addr, off, size, _, _ in sections:
        if flags & SHF_ALLOC and addr <= base and base + 4 * count <= addr + size:
            return list(struct.unpack_from(f"<{count}I", image, off + base - addr))
    return None


def may_return(functions, ends, pointers):
    """The functions that may return, by a ret, an mret, or a tail call to one that may.

    Running off a function's end into the next, as assembly does, is a tail call.
    A call to any other function is a block's last instruction:
    the compiler puts unrelated code after it.
    A jr the tool cannot follow counts as a return, so a function is taken
    never to return only when it plainly does not, and no edge that exists is dropped.
    """
    tails = {}
    returns = set()
    for a, _, insns in functions:
        tails[a] = set()
        for pc, mnem, ops in insns:
            m = TARGET.search(ops)
            target = int(m.group(1), 16) if m else None
            if mnem in ("ret", "mret") or (mnem == "jr" and not pointers.get(a)):
                returns.add(a)
            elif mnem == "j" and target is not None and not a <= target < ends[a]:
                tails[a].add(target)
    # Past a last call, a function runs on only if the callee returns.
    last_call = {}
    for a, _, insns in functions:
        if not insns or insns[-1][1] in TERMINATORS:
            continue
        m = TARGET.search(insns[-1][2])
        if insns[-1][1] == "jal" and m:
            last_call[a] = int(m.group(1), 16)
        else:
            tails[a].add(ends[a])
    changed = True
    while changed:
        changed = False
        for a, targets in tails.items():
            if a in last_call and last_call[a] in returns:
                targets.add(ends[a])
            if a not in returns and targets & returns:
                returns.add(a)
                changed = True
    return returns


def blocks_of(fn_addr, fn_end, insns, pointers, returns, image, sections):
    """The function's blocks by start address, and each block's successors."""
    leaders = {fn_addr}
    jumps = {}
    leaving = set()
    tails = {}
    indirect = set()
    for i, (pc, mnem, ops) in enumerate(insns):
        m = TARGET.search(ops)
        target = int(m.group(1), 16) if m else None
        inside = target is not None and fn_addr <= target < fn_end
        nxt = insns[i + 1][0] if i + 1 < len(insns) else None
        if BRANCH.match(mnem):
            jumps[pc] = [target, nxt]
        elif mnem == "jal" and target is not None and target not in returns:
            jumps[pc] = []
        elif mnem == "j":
            jumps[pc] = [target] if inside else []  # outside is a tail call
            if not inside:
                tails[pc] = target
                if target in returns:
                    leaving.add(pc)
        elif mnem == "jr":
            table = None if ops.strip() == "ra" else jump_table(image, sections, insns, i)
            if table is not None and all(fn_addr <= t < fn_end for t in table):
                jumps[pc] = sorted(set(table))
            else:
                jumps[pc] = [] if ops.strip() == "ra" else sorted(pointers.get(fn_addr, ()))
            if not jumps[pc]:
                leaving.add(pc)
                if ops.strip() != "ra":
                    indirect.add(pc)
        elif mnem in ("ret", "mret"):
            jumps[pc] = []
            leaving.add(pc)
        else:
            continue
        for t in jumps[pc]:
            if t is not None and fn_addr <= t < fn_end:
                leaders.add(t)
        if nxt is not None:
            leaders.add(nxt)

    blocks = {}
    current = None
    for i, (pc, mnem, ops) in enumerate(insns):
        if pc in leaders:
            if current is not None:
                current.succ.add(pc)  # falls through
            current = blocks[pc] = Block(pc)
        current.end = insns[i + 1][0] if i + 1 < len(insns) else fn_end
        if mnem in ("jal", "jalr"):
            m = TARGET.search(ops)
            current.calls.append((pc, int(m.group(1), 16) if m else None))
            current.indirect |= m is None
        if pc in tails:
            current.tails.append((pc, tails[pc]))
        current.indirect |= pc in indirect
        if pc in jumps:
            current.succ.update(t for t in jumps[pc] if t is not None and fn_addr <= t < fn_end)
            current.leaves = pc in leaving
            current = None
    if current is not None and fn_end in returns:
        current.leaves = True  # runs into the next function
    return blocks


def halting(blocks):
    """The blocks from which no path returns: the machine stops there, or never leaves."""
    preds = {b: set() for b in blocks}
    for b, blk in blocks.items():
        for s in blk.succ:
            preds[s].add(b)
    live = {b for b, blk in blocks.items() if blk.leaves}
    work = list(live)
    while work:
        for p in preds[work.pop()]:
            if p not in live:
                live.add(p)
                work.append(p)
    return set(blocks) - live


def sccs(nodes, succ):
    """The strongly connected components among nodes, by Tarjan's algorithm without recursion."""
    index, low, on, stack, out = {}, {}, set(), [], []
    for root in sorted(nodes):
        if root in index:
            continue
        work = [(root, iter(sorted(succ[root] & nodes)))]
        index[root] = low[root] = len(index)
        stack.append(root)
        on.add(root)
        while work:
            v, it = work[-1]
            w = next(it, None)
            if w is not None:
                if w not in index:
                    index[w] = low[w] = len(index)
                    stack.append(w)
                    on.add(w)
                    work.append((w, iter(sorted(succ[w] & nodes))))
                elif w in on:
                    low[v] = min(low[v], index[w])
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[v])
            if low[v] == index[v]:
                comp = set()
                while True:
                    w = stack.pop()
                    on.discard(w)
                    comp.add(w)
                    if w == v:
                        break
                out.append(comp)
    return out


def loops_of(blocks, entry):
    """The loops as (header, body), outermost first.

    A loop is a cycle of blocks, a strongly connected component;
    its headers are the blocks entered from outside it,
    and the loops inside it are the components left once the edges back to its headers go.
    For a cycle with one entry this is the natural loop of that header,
    all its back edges together;
    a cycle with more entries is a loop all the same, named after its lowest header.
    """
    succ = {b: set(blk.succ) for b, blk in blocks.items()}
    reach = {entry}
    work = [entry]
    while work:
        for s in succ[work.pop()]:
            if s not in reach:
                reach.add(s)
                work.append(s)
    loops = []
    todo = [(reach, {})]
    while todo:
        nodes, cut = todo.pop()
        edges = {b: succ[b] - cut.get(b, set()) for b in nodes}
        for comp in sccs(nodes, edges):
            if len(comp) == 1 and not (edges[next(iter(comp))] & comp):
                continue
            outside = {p for p in reach - comp if succ[p] & comp}
            headers = {b for b in comp if any(b in succ[p] for p in outside)} or {min(comp)}
            loops.append((min(headers), comp))
            inner = {b: set(cut.get(b, set())) | (succ[b] & headers) for b in comp}
            todo.append((comp, inner))
    return sorted(loops, key=lambda hl: -len(hl[1]))


def reached(graph, by_name, blocks_by_fn, pointed):
    """The functions a trap reaches without passing through a cut or a halting block.

    The calls are read from the blocks, so a jr through a function's own jump table
    is a jump inside it, and a call through a register reaches every function in pointed.
    """
    todo = [by_name[n] for n in ENTRIES]
    seen = set(todo)
    while todo:
        a = todo.pop()
        blocks = blocks_by_fn.get(a, {})
        stops = halting(blocks)
        callees = set()
        for b, blk in blocks.items():
            if b in stops:
                continue
            callees |= {c for _, c in blk.calls + blk.tails if c is not None}
            if blk.indirect:
                callees |= set(pointed)
        for c in callees:
            if c in graph and c not in seen and graph[c].name not in CUTS:
                seen.add(c)
                todo.append(c)
    return seen


def match(src, frames_of, body, cut):
    """The source loop a loop of the image is, as (loop, frames of one instruction, frame index),
    or a reason there is none; None for a loop that only the cut runs."""
    paths = []
    sample = None
    for a in body:
        frames = frames_of.get(a)
        if not frames or any(f.pos is None for f in frames):
            continue
        if any(f.function in cut for f in frames):
            paths.append(None)
            continue
        path = []
        for i, f in enumerate(frames):
            if f.pos.file not in src.functions:
                return (f"its code comes from {f.pos}, a file no source read defines a function in; "
                        "the debug information and the sources disagree on where the tree is")
            path += [(l, i) for l in src.enclosing(f.pos)]
        paths.append(path)
        sample = sample or frames
    live = [p for p in paths if p is not None]
    if paths and not live:
        return None
    if not live:
        return "no instruction of it has a line"
    prefix = live[0]
    for p in live[1:]:
        n = 0
        while n < min(len(prefix), len(p)) and prefix[n][0] is p[n][0]:
            n += 1
        prefix = prefix[:n]
    if not prefix:
        places = sorted({repr(fr[-1].pos) for fr in (frames_of[a] for a in body)
                         if fr and fr[-1].pos})[:3]
        return "no loop of the source holds all of it, which comes from " + ", ".join(places)
    loop, index = prefix[-1]
    return loop, sample, index


class Checker:
    def __init__(self, src):
        self.src = src
        self.problems = []
        self.parametric = set()  # functions the image holds out of line, bounded by an argument
        self.used = []  # the annotations that bound something

    def discharge(self, frames, i, what):
        """The call at frames[i] must say what bounds it, or pass the question outward."""
        f = frames[i]
        note = self.src.call_note(f.pos)
        if note is None:
            self.problems.append(f"{f.function} at {f.pos} calls {what} "
                                 "with nothing to say what bounds it")
            return
        self.used.append(note)
        if note.kind != "arg":
            return
        if note.name != f.function:
            self.problems.append(f"{note} names {note.name}, but stands in {f.function}")
        elif i > 0:
            self.discharge(frames, i - 1, f.function)
        else:
            self.parametric.add(f.function)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("--symbolizer", default="llvm-symbolizer")
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--cflags", default="")
    ap.add_argument("--sources", required=True)
    ap.add_argument("--design", required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("elf")
    ap.add_argument("su", nargs="+")
    args = ap.parse_args()

    try:
        image, sections, _, _, functions, graph, pointed = load(args.objdump, args.elf, args.su)
        by_name = {fn.name: a for a, fn in graph.items()}
        starts = [f[0] for f in functions]
        ends = dict(zip(starts, starts[1:] + [text_end(sections)]))
        pointers = {}
        for w in data_words(image, sections):
            i = bisect.bisect_right(starts, w) - 1
            if i >= 0 and w < ends[starts[i]]:
                pointers.setdefault(starts[i], set()).add(w)
        returns = may_return(functions, ends, pointers)
        insns_of = {a: insns for a, _, insns in functions}
        blocks_by_fn = {a: blocks_of(a, ends[a], insns, pointers, returns, image, sections)
                        for a, _, insns in functions if insns}
        checked = sorted(reached(graph, by_name, blocks_by_fn, pointed) & blocks_by_fn.keys())

        # The loops of the image, with their instructions, and every call a trap may make.
        found = []  # (function, header, set of block starts, [instruction addresses])
        calls = []  # (function, pc, callee)
        for a in checked:
            blocks = blocks_by_fn[a]
            stops = halting(blocks)
            for h, body in loops_of(blocks, a):
                if h in stops:
                    continue
                addrs = [pc for pc, _, _ in insns_of[a]
                         if any(blocks[b].start <= pc < blocks[b].end for b in body)]
                found.append((a, h, body, addrs))
            for b, blk in blocks.items():
                calls += [(a, pc, c) for pc, c in blk.calls + blk.tails if c is not None]

        src = Source()
        src.read(args.cc, args.cflags, shlex.split(args.sources))
        frames_of = symbolize(args.symbolizer, args.elf,
                              [x for f in found for x in f[3]] + [pc for _, pc, _ in calls])

        check = Checker(src)
        matched = {}  # (function, header, body) -> source loop
        bound = []    # (function, header, source loop), for the summary
        for a, h, body, addrs in found:
            m = match(src, frames_of, addrs, CUTS)
            where = f"{graph[a].name} has a loop at {h:#x}"
            if m is None:
                continue
            if isinstance(m, str):
                check.problems.append(f"{where}: {m}")
                continue
            loop, frames, index = m
            for (a2, h2, body2), other in matched.items():
                if a2 == a and other is loop and (body < body2 or body2 < body):
                    check.problems.append(f"{where} and another at {h2:#x}, one inside the other, "
                                          f"both match {loop}")
            matched[(a, h, frozenset(body))] = loop
            if not loop.notes:
                check.problems.append(f"{where}, {loop}, which says nothing of its bound")
                continue
            bound.append((a, h, loop))
            check.used += loop.notes
            for note in loop.notes:
                if note.kind != "arg":
                    continue
                if note.name != loop.function:
                    check.problems.append(f"{note} names {note.name}, but stands in {loop.function}")
                elif index > 0:
                    check.discharge(frames, index - 1, loop.function)
                else:
                    check.parametric.add(loop.function)

        # Calls to a function bounded by an argument, until no new such function turns up.
        done = set()
        while check.parametric - done:
            name = sorted(check.parametric - done)[0]
            done.add(name)
            target = by_name.get(name)
            for a, pc, callee in calls:
                if callee == target and frames_of.get(pc) and frames_of[pc][-1].pos:
                    check.discharge(frames_of[pc], len(frames_of[pc]) - 1, name)
                elif callee == target:
                    check.problems.append(f"{graph[a].name} at {pc:#x} calls {name}, "
                                          "and the call has no line")

        # A cut is trusted only where the source guards each call to it.
        for cut, guard in CUTS.items():
            sites = [c for c in src.calls.values() if c.callee == cut]
            if not sites:
                check.problems.append(f"the cut {cut} is called nowhere in the sources read")
            for c in sites:
                if guard not in c.guards:
                    check.problems.append(f"{c.function} calls {cut} at {c.pos} "
                                          f"outside an if on {guard}, so the cut does not hold there")

        # A bound the loop's shape limits is proven, or false; any other rests on an invariant,
        # which the harness fuzz-work counts.
        proven = set()
        for _, _, loop in bound:
            most = counted(loop)
            for n in loop.notes:
                if n.kind != "bound" or most is None or n.value is None:
                    continue
                if most > n.value:
                    check.problems.append(f"{loop} may run {most} times, and {n} says {n.value}")
                proven.add(id(n))

        rows = walk_rows(args.design)
        walks = {n.name for n in src.notes if n.kind == "walk"}
        used = {n.name for n in check.used if n.kind in ("walk", "paid")}
        for w in sorted(walks - rows):
            check.problems.append(f"the walk {w} is not in the table of DESIGN.md, \"Bounded work\"")
        for w in sorted(rows - used):
            check.problems.append(f"the table of DESIGN.md, \"Bounded work\", lists {w}, "
                                  "which no loop a trap runs walks or pays for")
        for n in src.notes:
            if n.kind == "paid" and n.unit not in PAID_UNITS:
                check.problems.append(f"{n} counts in no unit host/work.c knows: "
                                      + ", ".join(sorted(PAID_UNITS)))
            if not n.call and n.loop is None:
                check.problems.append(f"{n} stands in no loop")
            if n.call and n.target is None:
                check.problems.append(f"{n} has no statement after it")

        if check.problems:
            raise Failure("\n  ".join(["on a trap:"] + sorted(set(check.problems),
                                                               key=check.problems.index)))
        kinds = {}
        for _, _, loop in bound:
            for n in loop.notes:
                kinds[n.kind] = kinds.get(n.kind, 0) + 1
        shaped = sum(id(n) in proven for _, _, loop in bound for n in loop.notes)
        summary = ", ".join(f"{n} {k}" + (f" ({shaped} by their own shape)" if k == "bound" else "")
                            for k, n in sorted(kinds.items()))
        print(f"kernel loops: {len(bound)} on a trap, {summary}; "
              f"{len({id(n) for n in check.used if n.call})} calls bounded where they are made")
        if args.verbose:
            for a, h, loop in bound:
                print(f"  {graph[a].name} {h:#x}: {loop.begin} " +
                      "; ".join(f"{n.kind} {n.claim}" + ("" if id(n) in proven or n.kind != "bound"
                                                        else " (an invariant)") for n in loop.notes))
        return 0
    except Failure as e:
        print(f"loop-bounds: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
