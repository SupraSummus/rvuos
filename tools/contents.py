#!/usr/bin/env python3
"""Fail when the contents of MANUAL.md or DESIGN.md miss a section.

Each is a front page, and its other chapters are files in the directory of its name.
Its contents are the list items that are only a link,
and they must name every heading of the front page and the chapter files, from ## to ####,
in their order, by the anchor GitHub gives it,
since a section cited by its number or title is found through them.

Usage: contents.py FRONT.md...
"""

import re
import sys
from pathlib import Path

ITEM = re.compile(r"\s*(?:-|\d+\.) \[[^\]]+\]\(([^)#]*)#([^)]+)\)")
HEADING = re.compile(r"#{2,4} (.+)")


def anchors(path):
    """The anchor of each heading outside code, in order."""
    seen, out, code = {}, [], False
    for line in path.read_text().splitlines():
        code ^= line.startswith("```")
        m = None if code else HEADING.fullmatch(line)
        if m:
            a = re.sub(r"[^\w\- ]", "", m.group(1).lower()).replace(" ", "-")
            n = seen.get(a, 0)
            seen[a] = n + 1
            out.append(a if n == 0 else f"{a}-{n}")
    return out


def check(front):
    links = [(m.group(1) or front, m.group(2))
             for m in map(ITEM.fullmatch, Path(front).read_text().splitlines()) if m]
    files = {front, *map(str, Path(Path(front).stem.lower()).glob("*.md"))}
    linked = list(dict.fromkeys(f for f, _ in links))
    problems = [f"{front}: the contents link to nothing in {f}" for f in sorted(files - set(linked))]
    want = [(f, a) for f in linked if f in files for a in anchors(Path(f))]
    problems += [f"{front}: the contents do not list {f}#{a}" for f, a in want if (f, a) not in links]
    problems += [f"{front}: the contents list {f}#{a}, which is no heading" for f, a in links if (f, a) not in want]
    if not problems and links != want:
        problems.append(f"{front}: the contents are in another order than the headings")
    return problems


if __name__ == "__main__":
    problems = [p for front in sys.argv[1:] for p in check(front)]
    print("\n".join(problems), file=sys.stderr, end="\n" if problems else "")
    sys.exit(1 if problems else 0)
