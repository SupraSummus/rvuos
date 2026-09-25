"""The kernel's source as tools/loop-bounds.py reads it, and the way back from the image to it.

The loops and the annotations come from clang's AST, dumped as JSON,
which gives every loop's range and every annotation's place,
including those that macros and headers bring in.
An annotation is a _Static_assert whose message starts with "loop " or "call ",
so it costs no instruction and the compiler checks its condition; see kernel/work.h.
The way back is llvm-symbolizer, which gives each address its inlined frames.
"""

import json
import os
import shlex
import subprocess

from kimage import Failure

LOOPS = ("ForStmt", "WhileStmt", "DoStmt")


class Pos:
    """A place in a file, by line and column; ordered, and a range test beside it."""

    def __init__(self, file, line, col):
        self.file, self.line, self.col = os.path.realpath(file), line, col

    def key(self):
        return (self.line, self.col)

    def __repr__(self):
        return f"{short(self.file)}:{self.line}:{self.col}"


def short(path):
    rel = os.path.relpath(path)
    return path if rel.startswith("..") else rel


def within(pos, begin, end):
    """True if pos lies in [begin, end]; on the last line any column counts,
    since a range ends at its last token's start and the debug information
    may place an instruction anywhere on that line.
    A column of 0 is none, and the line alone decides."""
    if pos.file != begin.file or not begin.line <= pos.line <= end.line:
        return False
    return pos.col == 0 or pos.key() >= begin.key()


class Loop:
    def __init__(self, begin, end, function, node):
        self.begin, self.end, self.function = begin, end, function
        self.node = node
        self.notes = []

    def __repr__(self):
        return f"the loop at {self.begin} in {self.function}"


class Note:
    """An annotation: its kind, its claim, where it stands, and in which function.

    name is the first word of the claim: a walk's row, a paid loop's, an argument's function.
    unit is what a paid loop takes away; value is a bound's, when literals make it.
    """

    def __init__(self, text, pos, function, loop, target, value):
        words = text.split(" ")
        self.call = words[0] == "call"
        self.kind = words[1]
        self.claim = " ".join(words[2:])
        self.name = words[2] if len(words) > 2 else ""
        self.unit = words[3] if self.kind == "paid" and len(words) > 3 else None
        self.pos, self.function, self.loop = pos, function, loop
        self.target = target  # for a call annotation, the statement after it, as (begin, end)
        self.value = value

    def __repr__(self):
        return f"{'call' if self.call else 'loop'} {self.kind} {self.claim} at {self.pos}"


def resolve_locations(tree):
    """Give every location of the dump its file and line.

    The dump leaves out a location's file and line when they are those of the location
    printed before it, so they are filled in walking the dump in the order it was written.
    """
    state = {"file": None, "line": None}
    stack = [tree]
    while stack:
        o = stack.pop()
        if isinstance(o, list):
            stack.extend(reversed(o))
        elif isinstance(o, dict):
            if "offset" in o and "col" in o:
                state["file"] = o.get("file", state["file"])
                state["line"] = o.get("line", state["line"])
                o["_pos"] = Pos(state["file"], state["line"], o["col"]) if state["file"] else None
                continue  # includedFrom inside names a file but is no location
            stack.extend(reversed(list(o.values())))


def place(loc):
    """Where a location stands in the file being read: a macro's use, not its definition."""
    if not loc:
        return None
    if "expansionLoc" in loc:
        return loc["expansionLoc"].get("_pos")
    return loc.get("_pos")


def message(node):
    """The message of a _Static_assert, or None."""
    inner = node.get("inner", [])
    for child in inner[1:]:
        if child.get("kind") == "StringLiteral":
            return json.loads(child["value"])
    return None


class Source:
    """Every loop and annotation of the kernel's translation units."""

    def __init__(self):
        self.loops = {}   # (file, line, col) of begin -> Loop
        self.notes = []
        self.seen = set()  # (place, message) of each note, as headers are read again
        self.functions = {}  # file -> [(begin, end, name)]
        self.calls = {}   # (callee, place) -> Call

    def read(self, cc, cflags, path):
        cmd = [cc] + shlex.split(cflags) + ["-fsyntax-only", "-Xclang", "-ast-dump=json", path]
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError) as e:
            raise Failure(f"cannot dump the AST of {path}: {getattr(e, 'stderr', e)}")
        tree = json.loads(out)
        resolve_locations(tree)
        self._walk(tree, None, [])

    def _walk(self, tree, function, loops):
        # Iterative, with the enclosing function, loops and if conditions carried along.
        stack = [(tree, function, loops, frozenset())]
        while stack:
            node, function, loops, guards = stack.pop()
            if not isinstance(node, dict):
                continue
            kind = node.get("kind")
            rng = node.get("range", {})
            begin, end = place(rng.get("begin")), place(rng.get("end"))
            if kind == "FunctionDecl" and "inner" in node and any(
                    c.get("kind") == "CompoundStmt" for c in node["inner"]):
                function = node.get("name")
                if begin and end:
                    self.functions.setdefault(begin.file, []).append((begin, end, function))
            if kind in LOOPS and begin and end:
                k = (begin.file, begin.line, begin.col)
                loop = self.loops.setdefault(k, Loop(begin, end, function, node))
                loops = loops + [loop]
            children = node.get("inner", [])
            if kind == "CallExpr" and children:
                callee = next((n for n in names(children[0]) if n), None)
                if callee and begin:
                    self.calls.setdefault((callee, repr(begin)), Call(callee, begin, function, guards))
            if kind == "IfStmt" and children:
                # The condition guards the branch taken when it holds, the first after it.
                cond = children[0]
                held = guards | frozenset(names(cond))
                branches = [c for c in children[1:] if c]
                for i, child in reversed(list(enumerate(branches))):
                    stack.append((child, function, loops, held if i == 0 else guards))
                stack.append((cond, function, loops, guards))
                continue
            if kind == "CompoundStmt":
                for i, child in enumerate(children):
                    for decl in child.get("inner", []) if child.get("kind") == "DeclStmt" else []:
                        if decl.get("kind") == "StaticAssertDecl":
                            self._note(decl, function, loops, children[i + 1:i + 2])
            for child in reversed(children):
                stack.append((child, function, loops, guards))

    def _note(self, decl, function, loops, following):
        text = message(decl)
        if text is None or not text.startswith(("loop ", "call ")):
            return
        pos = place(decl.get("range", {}).get("begin")) or place(decl.get("loc"))
        target = None
        if following:
            r = following[0].get("range", {})
            b, e = place(r.get("begin")), place(r.get("end"))
            if b and e:
                target = (b, e)
        loop = loops[-1] if loops else None
        if (repr(pos), text) in self.seen:
            return
        self.seen.add((repr(pos), text))
        # A bound's _Static_assert is (n) > 0.
        cond = strip(decl["inner"][0]) if decl.get("inner") else None
        bound = value(cond["inner"][0]) if cond and cond.get("opcode") == ">" else None
        note = Note(text, pos, function, loop, target, bound)
        self.notes.append(note)
        if loop is not None and not note.call:
            loop.notes.append(note)

    def enclosing(self, pos):
        """The loops around pos, outermost first."""
        around = [l for l in self.loops.values() if within(pos, l.begin, l.end)]
        return sorted(around, key=lambda l: (l.begin.key(), -l.end.line, -l.end.col))

    def call_note(self, pos):
        """The call annotation whose statement holds pos, or None."""
        for n in self.notes:
            if n.call and n.target and within(pos, *n.target):
                return n
        return None


class Call:
    """A call in the source: what it calls, where, and the names in the conditions around it."""

    def __init__(self, callee, pos, function, guards):
        self.callee, self.pos, self.function, self.guards = callee, pos, function, guards


def names(node):
    """The names every DeclRefExpr under node refers to, in order."""
    out = []
    stack = [node]
    while stack:
        n = stack.pop()
        if isinstance(n, dict):
            if n.get("kind") == "DeclRefExpr":
                out.append(n.get("referencedDecl", {}).get("name"))
            stack.extend(reversed(n.get("inner", [])))
    return out


def strip(node):
    """An expression without the casts and parentheses around it."""
    while node and node.get("kind") in ("ImplicitCastExpr", "CStyleCastExpr", "ParenExpr",
                                        "ConstantExpr") and node.get("inner"):
        node = node["inner"][0]
    return node


def value(node):
    """The value of an integer constant expression built of literals, or None."""
    node = strip(node)
    if not node:
        return None
    if node.get("kind") == "IntegerLiteral":
        return int(node["value"], 0)
    if node.get("kind") == "BinaryOperator" and len(node.get("inner", [])) == 2:
        a, b = (value(x) for x in node["inner"])
        if a is None or b is None:
            return None
        op = node.get("opcode")
        ops = {"+": lambda: a + b, "-": lambda: a - b, "*": lambda: a * b,
               "/": lambda: a // b if b else None, "<<": lambda: a << b, ">>": lambda: a >> b}
        return ops[op]() if op in ops else None
    return None


def variable(node):
    """The name of the variable an expression is, or None."""
    node = strip(node)
    if node and node.get("kind") == "DeclRefExpr":
        ref = node.get("referencedDecl", {})
        return ref.get("name") if ref.get("kind") in ("VarDecl", "ParmVarDecl") else None
    return None


def writes_to(node, var):
    """True if anything under node assigns var, steps it, or takes its address."""
    stack = [node]
    while stack:
        n = stack.pop()
        if not isinstance(n, dict):
            continue
        kind, inner = n.get("kind"), n.get("inner", [])
        if kind in ("BinaryOperator", "CompoundAssignOperator") and inner and (
                n.get("opcode", "").endswith("=") and n.get("opcode") not in ("==", "<=", ">=", "!=")
                and variable(inner[0]) == var):
            return True
        if kind == "UnaryOperator" and inner and n.get("opcode") in ("++", "--", "&") and (
                variable(inner[0]) == var):
            return True
        stack.extend(inner)
    return False


def counted(loop):
    """The most iterations a loop's own shape allows, or None when its shape does not say.

    The shape is for (v = start; ... v < limit ...; v++), the limit a constant,
    v unsigned or starting at a constant of at least zero,
    the condition a conjunction holding v < limit or v <= limit,
    and nothing but the step writing v.
    """
    node = loop.node
    if node.get("kind") != "ForStmt" or len(node.get("inner", [])) != 5:
        return None
    init, _, cond, inc, body = node["inner"]
    var = start = None
    unsigned = False
    if init.get("kind") == "DeclStmt" and len(init.get("inner", [])) == 1:
        decl = init["inner"][0]
        var = decl.get("name")
        qual = decl.get("type", {})
        unsigned = "unsigned" in qual.get("desugaredQualType", qual.get("qualType", "")) or \
            qual.get("qualType", "").startswith("uint")
        start = value(decl["inner"][0]) if decl.get("inner") else None
    elif init.get("kind") == "BinaryOperator" and init.get("opcode") == "=":
        var, start = variable(init["inner"][0]), value(init["inner"][1])
        unsigned = "unsigned" in init.get("type", {}).get("desugaredQualType",
                                                          init.get("type", {}).get("qualType", ""))
    if var is None or not (unsigned or (start is not None and start >= 0)):
        return None

    def limit_of(c):
        c = strip(c)
        if c.get("kind") != "BinaryOperator":
            return None
        if c.get("opcode") == "&&":
            found = [limit_of(x) for x in c["inner"]]
            found = [f for f in found if f is not None]
            return min(found) if found else None
        if c.get("opcode") in ("<", "<=") and variable(c["inner"][0]) == var:
            n = value(c["inner"][1])
            return None if n is None else n + (c["opcode"] == "<=")
        return None

    def steps(i):
        i = strip(i)
        if i.get("kind") == "UnaryOperator" and i.get("opcode") == "++" and variable(i["inner"][0]) == var:
            return True
        if i.get("kind") == "CompoundAssignOperator" and i.get("opcode") == "+=" and (
                variable(i["inner"][0]) == var and (value(i["inner"][1]) or 0) >= 1):
            return True
        if i.get("kind") == "BinaryOperator" and i.get("opcode") == ",":
            a, b = i["inner"]
            return (steps(a) and not writes_to(b, var)) or (steps(b) and not writes_to(a, var))
        return False

    limit = limit_of(cond) if cond else None
    if limit is None or not inc or not steps(inc) or writes_to(body, var) or writes_to(cond, var):
        return None
    return max(0, limit - (start or 0)) if start is not None else limit


class Frame:
    def __init__(self, function, pos):
        self.function, self.pos = function, pos

    def __repr__(self):
        return f"{self.function} {self.pos}"


def symbolize(symbolizer, elf, addrs):
    """Each address's frames, outermost first: the function the image holds,
    then each function inlined into it down to the one the instruction came from.
    A frame without a line is None; the debug information lost it."""
    addrs = sorted(set(addrs))
    if not addrs:
        return {}
    try:
        out = subprocess.run([symbolizer, "--output-style=JSON", "--inlining", "-e", elf],
                             input="\n".join(f"{a:#x}" for a in addrs),
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise Failure(f"cannot run {symbolizer}: {e}")
    result = {}
    for a, line in zip(addrs, out.splitlines()):
        entry = json.loads(line)
        entry = entry[0] if isinstance(entry, list) else entry
        frames = []
        for s in reversed(entry.get("Symbol", [])):
            pos = Pos(s["FileName"], s["Line"], s["Column"]) if s.get("Line") else None
            frames.append(Frame(s.get("FunctionName"), pos))
        result[a] = frames
    return result

