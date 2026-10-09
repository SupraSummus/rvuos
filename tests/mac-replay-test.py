#!/usr/bin/env python3
"""What mac-replay-test steps over as left out by design, and what it still stops at.

Its places, test/replay-expected.txt's, let the own start's mac_config leave accesses of the libraries' out;
since the replay holds every step of that start, a place must step over their run alone and exactly:
a place met lets the case pass, once;
the same accesses not written down, or written down as another operation, still stop it;
a place their run does not hold fails as one not met;
a mask holds its bits alone;
and a place at their run's end is met as one in its middle.
Each case runs the built test on the recorded file with a few accesses of a word no code reaches put into it,
its places after the expected file's own, which the own start's skips already need.

Usage: tests/mac-replay-test.py BINARY DIR EXPECTED, DIR holding the recorded config.txt.
"""

import os
import subprocess
import sys
import tempfile

WORD = 0x600a4ffc  # a word neither the libraries' configuration nor macstart.c reaches
REASON = "# a place of the test's own\n"


def run(binary, recorded, at, put, expected):
    """The test's verdict and last line, on the recorded accesses with put's inserted before access at, or at the end."""
    lines = [l for l in open(recorded) if not l.startswith("#")]
    at = len(lines) if at is None else at
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "config.txt"), "w") as f:
            f.writelines(lines[:at] + ["%s4 0x%08x 0x%08x\n" % a for a in put] + lines[at:])
        with open(os.path.join(d, "expected.txt"), "w") as f:
            f.write(expected)
        r = subprocess.run([binary, d, os.path.join(d, "expected.txt")], capture_output=True, text=True)
    return r.returncode, r.stdout.strip().splitlines()[-1]


def place(*accesses):
    return REASON + "".join("-%s4 0x%08x %s\n" % a for a in accesses)


def main():
    binary, recorded = sys.argv[1], os.path.join(sys.argv[2], "config.txt")
    own = open(sys.argv[3]).read() + "\n"
    put = [("R", WORD, 0x12345678), ("W", WORD, 0x12345679)]
    cases = [
        ("a place met", 100, put, place(("R", WORD, "0x12345678"), ("W", WORD, "0x12345679")), 0),
        ("the same accesses not written down", 100, put, REASON, 1),
        ("a read written down as a write", 100, put, place(("W", WORD, "0x12345678"), ("W", WORD, "0x12345679")), 1),
        ("a place their run does not hold", None, [], place(("W", WORD, "0x00000001")), 1),
        ("a mask holding its bits alone", 100, put, place(("R", WORD, "0x00005678/0x0000ffff"),
                                                          ("W", WORD, "0x00000079/0x000000ff")), 0),
        ("a masked bit that differs", 100, put, place(("R", WORD, "0x00005678/0x0000ffff"),
                                                      ("W", WORD, "0x00000078/0x000000ff")), 1),
        ("a place at their run's end", None, put, place(("R", WORD, "0x12345678"), ("W", WORD, "0x12345679")), 0),
        ("a place met once, its accesses twice", 100, put + put,
         place(("R", WORD, "0x12345678"), ("W", WORD, "0x12345679")), 1),
    ]
    failures = 0
    for name, at, accesses, expected, want in cases:
        status, said = run(binary, recorded, at, accesses, own + expected)
        if (status == 0) != (want == 0):
            failures += 1
            print("mac-replay-test.py: FAIL %s: %s" % (name, said))
    if failures:
        sys.exit("mac-replay-test.py: %d failures" % failures)
    print("mac-replay-test.py: ok, %d cases" % len(cases))


if __name__ == "__main__":
    main()
