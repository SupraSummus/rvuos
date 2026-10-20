#!/usr/bin/env python3
"""The text logic of tools/mac-trace.py, on synthetic logs of a few lines each.

What the tool lets pass or holds back has to be what its help says:
a word any pair of dry runs differs in is volatile, even where two of them agree;
the bits MOMENT names stay out of compare's verdict, the rest of their word in;
and seq holds a run's MAC accesses on the start's thread to a gated run's, value by value, the moment's bits apart.
The ELF and the libraries are not read.
"""

import argparse
import contextlib
import importlib.util
import io
import os
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "mac-trace.py")
spec = importlib.util.spec_from_file_location("mac_trace", TOOL)
mt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mt)


def log(*lines):
    """A trace log's text: a record is (thread, op, address, value), its pc PC, or with a fifth item, that pc;
    a cycle is ("cycle", turns, length), and a string is a line as it stands;
    records and cycles take the next sequence numbers."""
    out = []
    seq = 0
    for item in lines:
        if isinstance(item, str):
            out.append(item)
            continue
        seq += 1
        if item[0] == "cycle":
            out.append("   1.000 [%d] cycle x%d of %d:" % (seq, item[1], item[2]))
        else:
            t, op, address, value = item[:4]
            pc = item[4] if len(item) > 4 else 0x42000010
            out.append("   1.000 [%d] pc 0x%08x t%d %s4 0x%08x = 0x%08x" % (seq, pc, t, op, address, value))
    return "\n".join(out) + "\n"


def snaps(words):
    return "snapshot of %d addresses\n" % len(words) + "".join("snap 0x%08x = 0x%08x\n" % w for w in words.items())


class Logs:
    """Files in a directory of their own, named by what they hold."""

    def __init__(self, path):
        self.path = path

    def __call__(self, name, text):
        p = os.path.join(self.path, name)
        with open(p, "w") as f:
            f.write(text)
        return p


def run(fn, **kw):
    """A subcommand's exit status and what it printed."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = fn(argparse.Namespace(**kw))
    return status, out.getvalue()


def compare(dry, traced):
    return run(mt.compare, dry=dry, traced=traced)


def seq(gated, own, thread=1):
    return run(mt.seq, gated=gated, own_log=own, thread=thread)


A, B, C, X, Y = (0x600A4000 + 4 * i for i in (0, 1, 2, 5, 6))
TX_BLOCK = 0x600A4CA8


def test_compare_volatile_and_the_moment(f):
    k = next(a for a, (mask, _) in mt.MOMENT.items() if mask == 0xFFFFFFFF)
    dry = [f("d1", snaps({X: 1, Y: 5, k: 10})), f("d2", snaps({X: 1, Y: 5, k: 10})), f("d3", snaps({X: 2, Y: 5, k: 10}))]
    status, out = compare(dry, f("t", snaps({X: 3, Y: 5, k: 12})))
    assert status == 0 and "1 volatile" in out and "1 of the moment, not compared" in out, out
    status, out = compare(dry, f("t2", snaps({X: 1, Y: 6, k: 10})))
    assert status == 1 and "divergent 0x%08x" % Y in out, out


def test_the_moments_bits_alone_are_read_apart(f):
    # TX_BLOCK's busy bits are the moment's, its other bits the code's.
    t, busy = TX_BLOCK, mt.MOMENT[TX_BLOCK][0]
    dry = [f("d%d" % i, snaps({t: 0x1000})) for i in range(2)]
    assert compare(dry, f("t", snaps({t: 0x1000 | busy})))[0] == 0
    assert compare(dry, f("t2", snaps({t: 0})))[0] == 1


def test_seq_holds_the_mac_stream(f):
    # One traced run against the gated one: the MAC's accesses on the start's thread, value by value, the moment's
    # bits and a poll's turns apart; another thread's accesses and libphy's words are not held.
    clock, phy = 0x600AD000, 0x600A0450
    gated = f("gated", log((1, "W", A, 1), ("cycle", 5, 1), (1, "R", B, 0), (1, "R", clock, 10), (2, "R", C, 7),
                           (1, "W", phy, 1)))
    same = f("same", log((1, "W", A, 1), ("cycle", 2, 1), (1, "R", B, 0), (1, "R", clock, 99), (2, "R", C, 8),
                         (1, "W", phy, 2)))
    status, out = seq(gated, same)
    assert status == 0 and "3 MAC accesses, the same" in out, out
    status, out = seq(gated, f("value", log((1, "W", A, 3), (1, "R", B, 0), (1, "R", clock, 10))))
    assert status == 1 and "part at access 1" in out, out
    status, out = seq(gated, f("short", log((1, "W", A, 1), (1, "R", clock, 10))))
    assert status == 1 and "part at access 2" in out, out
    status, out = seq(gated, f("longer", log((1, "W", A, 1), (1, "R", B, 0), (1, "R", clock, 10), (1, "W", A, 0))))
    assert status == 1 and "3 gated and 4 own" in out, out


def test_a_short_log_is_refused(f):
    lines = log((1, "W", A, 1), (1, "W", B, 1), (1, "W", C, 1)).splitlines()
    short = f("short", "\n".join([lines[0], lines[2]]) + "\n")
    try:
        seq(f("gated", log((1, "W", A, 1), (1, "W", B, 1), (1, "W", C, 1))), short)
    except SystemExit as e:
        assert "short" in str(e), e
    else:
        raise AssertionError("a log missing a sequence number passed")


def main():
    tests = [(n, t) for n, t in sorted(globals().items()) if n.startswith("test_")]
    failures = 0
    for name, test in tests:
        with tempfile.TemporaryDirectory() as d:
            try:
                test(Logs(d))
            except AssertionError as e:
                failures += 1
                print("mac-trace-test: FAIL %s\n%s" % (name, e))
    if failures:
        sys.exit("mac-trace-test: %d failures" % failures)
    print("mac-trace-test: ok, %d tests" % len(tests))


if __name__ == "__main__":
    main()
