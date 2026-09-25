#!/usr/bin/env python3
"""Write the patches of tests/mutants/ again against the kernel in the working tree.

Each hunk is found by the lines it changes, not by its context:
the context is dropped from the outside in until exactly one place matches,
and failing that, the indentation too, with the added lines moving along.
A patch is then
  refreshed  when its context matched, so only line numbers and context change,
  moved      when it did not, so run tests/mutants.sh on it and read its diff,
  stale      when its lines match nowhere or in several places; plant it again by hand.

Usage: tests/mutants-refresh.py [name...]
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MUTANTS = os.path.join(ROOT, "tests", "mutants")


class Stale(Exception):
    pass


def parse(text):
    """A patch as its header and a list of (path, hunks), a hunk being (start, body)."""
    at = text.find("diff --git ")
    header, files, hunks = text[:at], [], None
    for line in text[at:].split("\n"):
        if line.startswith("diff --git "):
            hunks = []
        elif line.startswith("@@"):
            hunks.append((int(re.match(r"@@ -(\d+)", line).group(1)) - 1, []))
        elif hunks:
            if line[:1] in (" ", "-", "+"):
                hunks[-1][1].append(line)
        elif line.startswith("+++ b/"):
            files.append((line[len("+++ b/"):], hunks))
    return header, files


def split(body):
    """A hunk's body as the context before, the lines it changes, old and new, and the context after."""
    lead = next(i for i, l in enumerate(body) if l[0] != " ")
    trail = len(body) - next(i for i, l in enumerate(reversed(body)) if l[0] != " ")
    core = body[lead:trail]
    return ([l[1:] for l in body[:lead]], [l[1:] for l in core if l[0] != "+"],
            [l[1:] for l in core if l[0] != "-"], [l[1:] for l in body[trail:]])


def indent(line):
    return len(line) - len(line.lstrip(" "))


def exact(have, want):
    return 0 if have == want else None


def shifted(have, want):
    """How far right the lines have moved, if that is all that changed them."""
    if any(h.strip() != w.strip() for h, w in zip(have, want)):
        return None
    shifts = {indent(h) - indent(w) for h, w in zip(have, want) if h.strip()}
    return None if len(shifts) > 1 else next(iter(shifts), 0)


def locate(lines, start, body):
    """Where the lines the hunk changes start now, how far right they moved,
    and whether they moved in a way their context did not."""
    before, old, _, after = split(body)
    full = len(before) + len(after)
    for match in (exact, shifted):
        # A hunk that only adds lines keeps one line of context to be found by.
        for total in range(full, -1 if old else 0, -1):
            places = {}
            for i in range(max(0, total - len(after)), min(len(before), total) + 1):
                want = before[len(before) - i:] + old + after[:total - i]
                for at in range(len(lines) - len(want) + 1):
                    shift = match(lines[at:at + len(want)], want)
                    if shift is not None:
                        places[at + i] = shift
            if len(places) > 1 and total == full:
                # As in git apply, the place nearest the old one breaks a tie of full context.
                was = start + len(before)
                near = sorted(places, key=lambda at: abs(at - was))
                if abs(near[0] - was) < abs(near[1] - was):
                    places = {near[0]: places[near[0]]}
            if len(places) > 1:
                raise Stale(f"its lines match in {len(places)} places")
            if places:
                at, shift = places.popitem()
                return at, shift, total < full or match is shifted
    raise Stale("its lines match nowhere")


def carry(lines, hunks):
    """The lines with every hunk's change made where it is found, and whether any hunk moved."""
    edits, moved = [], False
    for start, body in hunks:
        at, shift, lost = locate(lines, start, body)
        _, old, new, _ = split(body)
        if any(indent(l) + shift < 0 for l in new if l.strip()):
            raise Stale("its added lines do not move with the rest")
        new = [" " * (indent(l) + shift) + l.lstrip(" ") if l.strip() else l for l in new]
        edits.append((at, len(old), new))
        moved |= lost
    out, pos = [], 0
    for at, size, new in sorted(edits):
        if at < pos:
            raise Stale("two hunks land on the same lines")
        out += lines[pos:at] + new
        pos = at + size
    return out + lines[pos:], moved


def diff(path, old, new):
    """What git diff writes for the change, without the index line."""
    with tempfile.TemporaryDirectory() as tmp:
        for side, lines in (("a", old), ("b", new)):
            os.makedirs(os.path.join(tmp, side, os.path.dirname(path)), exist_ok=True)
            with open(os.path.join(tmp, side, path), "w") as f:
                f.write("".join(l + "\n" for l in lines))
        out = subprocess.run(
            ["git", "diff", "--no-index", "--no-prefix", "--no-color", "--no-ext-diff", "-U3",
             f"a/{path}", f"b/{path}"], cwd=tmp, capture_output=True, text=True).stdout
    return re.sub(r"^index .*\n", "", out, count=1, flags=re.M)


def refresh(patch):
    """Write the patch again, and say whether it was fresh, refreshed or moved."""
    with open(patch) as f:
        text = f.read()
    header, files = parse(text)
    body, moved = "", False
    for path, hunks in files:
        try:
            with open(os.path.join(ROOT, path)) as f:
                lines = f.read().splitlines()
        except FileNotFoundError:
            raise Stale(f"{path} is gone")
        new, lost = carry(lines, hunks)
        body += diff(path, lines, new)
        moved |= lost
    if header + body == text:
        return "fresh"
    with open(patch, "w") as f:
        f.write(header + body)
    return "moved" if moved else "refreshed"


def main(names):
    stale = False
    for name in names or sorted(n.removesuffix(".patch") for n in os.listdir(MUTANTS)):
        name = os.path.basename(name).removesuffix(".patch")
        try:
            state = refresh(os.path.join(MUTANTS, name + ".patch"))
        except (Stale, FileNotFoundError) as e:
            state, stale = f"stale, {e}", True
        if state != "fresh":
            print(f"mutant {name}: {state}")
    return stale


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
