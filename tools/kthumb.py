"""The kernel image in Thumb-2, as tools/stack-depth.py and tools/loop-bounds.py read it on ARMv7-M.

kimage.py reads RISC-V; this file is its counterpart for the one other instruction set,
and each tool asks kimage.arch which of the two an image holds.
What the tools need of an instruction is where it may go next,
what it writes, and what a load reads from; the rest of the tools is the same for both.

A control transfer is one of these kinds:
  jump     b, to its target alone
  branch   b<cond>, cbz, cbnz: to its target or the next instruction
  call     bl, to a function that returns to the next instruction
  icall    blx through a register
  ijump    bx through a register other than lr, or a load or move into pc
  return   bx lr, or a pop or an ldm off sp that loads pc; the exception return of the trap glue is one
  table    tbb and tbh, through the table of offsets that follows them
Inside an IT block the disassembler writes the condition into the mnemonic,
so a conditional return or call falls through to the next instruction as well.
A branch to another function is a tail call, conditional or not.
A function's address in a register or in memory carries bit 0, Thumb's, which the tools take off.
"""

import re

from kimage import Block, Failure, TARGET, imm

# Thumb's addressing: [base], [base, #offset], with a write-back after or outside the brackets.
BASED = re.compile(r"\[(\w+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]")
COND = "(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)"
_B = re.compile(rf"b({COND})?")
_BL = re.compile(rf"bl({COND})?")
_BLX = re.compile(rf"blx({COND})?")
_BX = re.compile(rf"bx({COND})?")
_POP = re.compile(rf"pop({COND})?")
_LDM = re.compile(rf"ldm(?:ia|fd|db|ea)?({COND})?")
_LDR = re.compile(rf"ldr({COND})?")
_MOV = re.compile(rf"movs?({COND})?")
LOAD = re.compile(rf"ldr(?:b|h|sb|sh|d|ex|exb|exh|t|bt|ht)?({COND})?(?:\.[wn])?$")
# A board whose SysTick wakes no wfe polls on yield; see intr_wait in kernel/arch/arm/trap.c.
WAITS = ("wfi", "wfe", "yield")

# Instructions that write no register named first; everything else writes its first operand.
NO_DEST = re.compile(rf"(?:str\w*|stm\w*|push|cmp|cmn|tst|teq|b|bl|blx|bx|cbz|cbnz|tbb|tbh|it[te]*|"
                     rf"nop|dsb|dmb|isb|msr|cpsi[de]|wfi|wfe|sev|bkpt|svc|clrex|udf|pld)({COND})?$")
TERMINATORS = ("jump", "return", "ijump", "table")


def bare(mnem):
    """The mnemonic without its width, .w or .n."""
    return re.sub(r"\.[wn]$", "", mnem)


def operands(ops):
    """The operands, without the disassembler's comment."""
    return ops.split("@", 1)[0].strip()


def args_of(ops):
    """The operands one by one; an address in brackets or a list in braces is one."""
    return [a.strip() for a in re.findall(r"(?:\[[^\]]*\]!?|\{[^}]*\}|[^,])+", operands(ops))]


def reglist(ops):
    m = re.search(r"\{([^}]*)\}", ops)
    return [r.strip() for r in m.group(1).split(",")] if m else []


def classify(mnem, ops):
    """The instruction's kind of control transfer, or None, and whether it is conditional."""
    m = bare(mnem)
    o = operands(ops)
    args = args_of(ops)

    def cond(match):
        return match.group(1) not in (None, "al")

    if (x := _B.fullmatch(m)):
        return ("branch" if cond(x) else "jump"), cond(x)
    if (x := _BL.fullmatch(m)):
        return "call", cond(x)
    if (x := _BLX.fullmatch(m)):
        return "icall", cond(x)
    if (x := _BX.fullmatch(m)):
        return ("return" if args == ["lr"] else "ijump"), cond(x)
    if m in ("cbz", "cbnz"):
        return "branch", True
    if m in ("tbb", "tbh"):
        return "table", False
    if (x := _POP.fullmatch(m)) and "pc" in reglist(o):
        return "return", cond(x)
    if (x := _LDM.fullmatch(m)) and "pc" in reglist(o):
        return ("return" if args and args[0].rstrip("!") == "sp" else "ijump"), cond(x)
    if (x := _LDR.fullmatch(m)) and args and args[0] == "pc":
        return "ijump", cond(x)
    if (x := _MOV.fullmatch(m)) and args and args[0] == "pc":
        return ("return" if args[1:] == ["lr"] else "ijump"), cond(x)
    return None, False


def in_it(insns, j):
    """Whether insns[j] runs only on a condition: an it, itt, ite and so on above it covers it.
    A block covers at most four and none nests, so the nearest it above within four decides."""
    for k in range(j - 1, max(j - 5, -1), -1):
        if re.fullmatch(r"it[te]{0,3}", insns[k][1]):
            return j - k < len(insns[k][1])
    return False


def predicated_exits(insns):
    """The instructions of every IT block that ends in a return, the it among them,
    and an unconditional branch right after one.
    Where RISC-V branches out of a loop to its function's return,
    Thumb may predicate the return inside the loop's block, and these come from after the loop,
    the branch that goes round again from where the return lies;
    tools/loop-bounds.py leaves them out when it matches a loop to the source."""
    out = set()
    for k, (pc, mnem, _) in enumerate(insns):
        if not re.fullmatch(r"it[te]{0,3}", mnem):
            continue
        last = k + len(mnem) - 1
        if last < len(insns) and classify(insns[last][1], insns[last][2])[0] == "return":
            out.update(insns[j][0] for j in range(k, last + 1))
            if last + 1 < len(insns) and bare(insns[last + 1][1]) == "b":
                out.add(insns[last + 1][0])
    return out


def base_of(insns, j):
    """The mnemonic of insns[j] without its width, and without the condition an IT block gives it."""
    m = bare(insns[j][1])
    return m[:-2] if in_it(insns, j) and re.search(rf"{COND}$", m) else m


def target(ops):
    m = TARGET.search(ops)
    return int(m.group(1), 16) if m else None


def writes(mnem, ops):
    """The registers an instruction writes, a set, pc and a base written back among them."""
    m = bare(mnem)
    o = operands(ops)
    out = set()
    if _POP.fullmatch(m) or _LDM.fullmatch(m):
        out.update(reglist(o))
    elif not NO_DEST.fullmatch(m):
        args = [a.strip() for a in re.sub(r"\{[^}]*\}|\[[^\]]*\]!?", "", o).split(",")]
        if args and args[0]:
            out.add(args[0])
        # umull and its kind write two, ldrd and ldrex's pair too
        if re.fullmatch(rf"(?:[us]mull|[us]mlal|ldrd)({COND})?s?", m) and len(args) > 1 and args[1]:
            out.add(args[1])
    if re.search(r"\]!|\], ", o) and (b := BASED.search(o)):
        out.add(b.group(1))
    elif (b := re.match(r"(\w+)!", o)):
        out.add(b.group(1))
    if m.startswith(("push", "pop")):
        out.add("sp")
    return out


def sp_taken(mnem, ops):
    """The bytes an instruction takes off sp, for the frame of an assembly function."""
    m = bare(mnem)
    o = operands(ops)
    if m == "push" or (m.startswith("stmdb") and o.startswith("sp!")):
        return 4 * len(reglist(o))
    args = args_of(ops)
    if m in ("sub", "subw") and args[0] == "sp" and args[-1].startswith("#"):
        if len(args) == 2 or args[1] == "sp":
            return imm(args[-1][1:])
    return 0


def literal(image, sections, addr, size=4):
    """The little-endian word, or halfword or byte, the image holds at addr, or None."""
    for _, _, flags, base, off, length, _, _ in sections:
        if flags & 0x2 and base <= addr and addr + size <= base + length:
            return int.from_bytes(image[off + addr - base:off + addr - base + size], "little")
    return None


def pc_value(pc):
    """What pc reads as in Thumb: the instruction's address plus four, word-aligned for a literal."""
    return (pc + 4) & ~3


def made(pc, m, args, image, sections):
    """The value an instruction writes into its first operand from constants alone, or None:
    a movw, a mov or mvn of an immediate, an adr, or a load of a literal."""
    if len(args) != 2:
        return None
    if args[1].startswith("#"):
        value = imm(args[1][1:])
        if m in ("movw", "mov", "movs"):
            return value & 0xFFFFFFFF
        if m in ("mvn", "mvns"):
            return ~value & 0xFFFFFFFF
        if m == "adr":
            return (pc_value(pc) + value) & 0xFFFFFFFF
    if m == "ldr" and (b := BASED.fullmatch(args[1])) and b.group(1) == "pc":
        return literal(image, sections, (pc_value(pc) + imm(b.group(2) or "0")) & 0xFFFFFFFF)
    return None


def fixed(insns, i, reg, image, sections):
    """The value an instruction above insns[i] left in reg, if it made one of nothing but constants,
    a movt with the movw above it among them.
    The nearest write to reg above decides, in the order of the addresses, and a conditional one makes it unknown."""
    for j in range(i - 1, -1, -1):
        pc, mnem, ops = insns[j]
        if reg not in writes(mnem, ops):
            continue
        if in_it(insns, j):
            return None
        m, args = bare(mnem), args_of(ops)
        if m == "movt" and len(args) == 2 and args[1].startswith("#"):
            low = fixed(insns, j, reg, image, sections)
            return None if low is None else ((imm(args[1][1:]) << 16) | (low & 0xFFFF)) & 0xFFFFFFFF
        return made(pc, m, args, image, sections)
    return None


def formed(insns, image, sections):
    """Every address the function forms from constants, for the call graph's taken addresses:
    a movw, alone and with each movt of its register after it, an adr and a literal load,
    as RISC-V forms one with lui or auipc, conditional ones as well, each with bit 0 taken off.
    A movt completes the movw that is the nearest write of its register above it alone,
    where neither is conditional, nothing branches to the movt or to what lies between,
    and the function jumps through no register,
    since every way to the movt then runs through the movw, as clang lays a constant out,
    right before the movt or with other instructions scheduled between;
    so a movw of a mask the register held earlier does not pair with it.
    A mov or mvn of an immediate forms a small number, as li does, however often it equals a function's address."""
    out = set()
    lows = {}
    landings = set()
    jumps_through = False
    for i, (pc, mnem, ops) in enumerate(insns):
        kind, _ = classify(mnem, ops)
        if kind in ("jump", "branch") and (t := target(ops)) is not None:
            landings.add(t)
        elif kind == "table":
            landings.update(table(insns, i, image, sections))
        elif kind == "ijump":
            jumps_through = True
    for i, (pc, mnem, ops) in enumerate(insns):
        m, args = base_of(insns, i), args_of(ops)
        if m == "movt" and len(args) == 2 and args[1].startswith("#"):
            high = imm(args[1][1:]) << 16
            j = next((j for j in range(i - 1, -1, -1) if args[0] in writes(insns[j][1], insns[j][2])), None)
            before = args_of(insns[j][2]) if j is not None else []
            if (j is not None and not jumps_through and not in_it(insns, i) and not in_it(insns, j)
                    and not any(insns[k][0] in landings for k in range(j + 1, i + 1))
                    and base_of(insns, j) == "movw" and len(before) == 2 and before[0] == args[0]
                    and before[1].startswith("#")):
                out.add((high | imm(before[1][1:])) & 0xFFFFFFFF)
            else:
                out.update((high | low) & 0xFFFFFFFF for low in lows.get(args[0], ()))
        elif m in ("movw", "adr", "ldr") and (v := made(pc, m, args, image, sections)) is not None:
            out.add(v)
            if m == "movw":
                lows.setdefault(args[0], set()).add(v & 0xFFFF)
    return out | {v & ~1 for v in out}


def table(insns, i, image, sections):
    """The targets of the tbb or tbh at insns[i]: its table of offsets lies right after it,
    up to the next instruction, each target that many halfwords past the table's start."""
    pc, mnem, _ = insns[i]
    start = pc + 4
    end = insns[i + 1][0] if i + 1 < len(insns) else start
    size = 1 if bare(mnem) == "tbb" else 2
    out = []
    for at in range(start, end - size + 1, size):
        entry = literal(image, sections, at, size)
        if entry is None:
            raise Failure(f"the table of the {mnem} at {pc:#x} lies outside the image")
        t = start + 2 * entry
        if not start <= t < end:
            out.append(t)
    return out


def may_return(functions, ends, pointers):
    """The functions that may return; see may_return in tools/loop-bounds.py, whose rules these are."""
    tails = {}
    returns = set()
    for a, _, insns in functions:
        tails[a] = set()
        for pc, mnem, ops in insns:
            kind, _ = classify(mnem, ops)
            t = target(ops) if kind in ("jump", "branch") else None
            if kind == "return" or (kind == "ijump" and not pointers.get(a)):
                returns.add(a)
            elif t is not None and not a <= t < ends[a]:
                tails[a].add(t)
    last_call = {}
    for a, _, insns in functions:
        if not insns:
            continue
        kind, cond = classify(insns[-1][1], insns[-1][2])
        if kind in TERMINATORS and not cond:
            continue
        t = target(insns[-1][2])
        if kind == "call" and not cond and t is not None:
            last_call[a] = t
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
    """The function's blocks by start address, and each block's successors; see blocks_of in tools/loop-bounds.py."""
    leaders = {fn_addr}
    jumps = {}
    leaving = set()
    tails = {}
    indirect = set()
    for i, (pc, mnem, ops) in enumerate(insns):
        kind, cond = classify(mnem, ops)
        t = target(ops) if kind in ("jump", "branch", "call") else None
        inside = t is not None and fn_addr <= t < fn_end
        nxt = insns[i + 1][0] if i + 1 < len(insns) else None
        after = [nxt] if cond else []
        if kind in ("jump", "branch"):
            if inside:
                jumps[pc] = [t] + after
            else:
                # To another function: a tail call, which leaves if the callee returns.
                jumps[pc] = after
                tails[pc] = t
                if t in returns:
                    leaving.add(pc)
        elif kind == "call" and t is not None and t not in returns and not cond:
            jumps[pc] = []
        elif kind == "table":
            jumps[pc] = sorted(set(table(insns, i, image, sections)))
        elif kind == "ijump":
            jumps[pc] = sorted(pointers.get(fn_addr, ())) + after
            if not pointers.get(fn_addr):
                leaving.add(pc)
                indirect.add(pc)
        elif kind == "return":
            jumps[pc] = after
            leaving.add(pc)
        else:
            continue
        for x in jumps[pc]:
            if x is not None and fn_addr <= x < fn_end:
                leaders.add(x)
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
        kind, _ = classify(mnem, ops)
        if kind in ("call", "icall"):
            current.calls.append((pc, target(ops) if kind == "call" else None))
            current.indirect |= kind == "icall"
        if pc in tails:
            current.tails.append((pc, tails[pc]))
        current.indirect |= pc in indirect
        if pc in jumps:
            current.succ.update(x for x in jumps[pc] if x is not None and fn_addr <= x < fn_end)
            current.leaves = pc in leaving
            current = None
    if current is not None and fn_end in returns:
        current.leaves = True  # runs into the next function
    return blocks


def waiting_blocks(blocks, insns, ram, waits, moving, image, sections):
    """The blocks that wait; see waiting_blocks in tools/loop-bounds.py.
    A load waits when its base register holds a fixed address outside RAM, as a device register lies."""
    def waits_here(i):
        pc, mnem, ops = insns[i]
        if bare(mnem) in WAITS:
            return True
        if not LOAD.match(mnem) or pc in moving:
            return False
        b = BASED.search(operands(ops))
        if not b or b.group(1) == "pc":
            return False
        base = fixed(insns, i, b.group(1), image, sections)
        return base is not None and not ram[0] <= (base + imm(b.group(2) or "0")) & 0xFFFFFFFF < ram[1]

    index = {pc: i for i, (pc, _, _) in enumerate(insns)}
    return {b for b, blk in blocks.items()
            if any(c in waits for _, c in blk.calls + blk.tails)
            or any(waits_here(i) for i in range(index[blk.start], index.get(blk.end, len(insns))))}


def moving_loads(inside):
    """The loads of a loop whose base register the loop writes, which read no fixed address."""
    written = set().union(*(writes(m, o) for _, m, o in inside)) if inside else set()
    return {pc for pc, m, o in inside
            if LOAD.match(m) and (b := BASED.search(operands(o))) and b.group(1) in written}


def analyse(functions, end, frames, image, sections, owner, by_addr):
    """Frames, edges and taken addresses; see analyse in tools/kimage.py, whose rules these are."""
    taken = set()
    for addr, name, insns in functions:
        fn = by_addr[addr]
        measured = 0
        for pc, mnem, ops in insns:
            kind, _ = classify(mnem, ops)
            t = target(ops) if kind in ("jump", "branch", "call") else None
            if t is not None:
                home = owner(t)
                if home is None:
                    raise Failure(f"{name} at {pc:#x} jumps out of the kernel's code")
                if t == addr and kind == "call":
                    fn.calls.add(t)
                elif home != addr:
                    if t != home:
                        raise Failure(f"{name} at {pc:#x} jumps into the middle of {by_addr[home].name}")
                    fn.calls.add(t)
            elif kind == "icall":
                fn.indirect_call = True
            elif kind == "ijump":
                fn.indirect_jump = True
            measured += sp_taken(mnem, ops)
        taken |= formed(insns, image, sections)
        fn.frame = frames.get(name, measured)
    return taken
