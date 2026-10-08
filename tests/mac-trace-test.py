#!/usr/bin/env python3
"""The text logic of tools/mac-trace.py, on synthetic logs of a few lines each.

The tool is the oracle of every step of the ESP32-C6's own bring-up,
so what it lets pass or holds back has to be what its help says:
a polling loop's turns are a note, not a difference;
a difference does not hide what comes after it;
a word any pair of base or dry runs differs in is volatile, even where two of them agree;
an interrupt thread's accesses are a multiset;
--join makes one stream of every thread;
--from aligns two runs by their phase markers, a cycle cut at the marker keeping its share;
a short log is refused, and with --image one that flashed another build;
and the words COUNTERS names stay out of compare's verdict.
The ELF and the libraries are not read: every pc is given to one library.
"""

import argparse
import contextlib
import hashlib
import importlib.util
import io
import os
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "mac-trace.py")
spec = importlib.util.spec_from_file_location("mac_trace", TOOL)
mt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mt)
mt.classifier = lambda args, pcs: (lambda pc: "lib", {})

PC = 0x42000010


def log(*lines):
    """A trace log's text: a record is (thread, op, address, value), a cycle is ("cycle", turns, length),
    and a string is a line as it stands; records and cycles take the next sequence numbers."""
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
            t, op, address, value = item
            out.append("   1.000 [%d] pc 0x%08x t%d %s4 0x%08x = 0x%08x" % (seq, PC, t, op, address, value))
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


def diff(bases, own, **kw):
    args = dict(base=bases, own_log=own, from_=None, to=None, join=False, collapse=[], elf="", rom="", lib=[], own=[],
                image=None)
    args.update(kw)
    return run(mt.diff, **args)


A, B, C, P, V, X, Y = (0x600A4000 + 4 * i for i in range(7))


def test_polling_is_a_note(f):
    base = f("base", log((1, "W", A, 1), ("cycle", 5, 1), (1, "R", P, 0), (1, "W", B, 1)))
    own = f("own", log((1, "W", A, 1), ("cycle", 9, 1), (1, "R", P, 0), (1, "W", B, 1)))
    status, out = diff([base, base], own)
    assert status == 0, out
    assert "polling" in out and "x5 vs x9" in out, out


def test_every_difference_is_shown(f):
    # A write of the own start's own value, and later an access the libraries' start does not make:
    # two places, the second after the first, each shown.
    base = f("base", log((1, "W", A, 1), (1, "W", B, 1), (1, "W", C, 1), (1, "W", P, 1)))
    own = f("own", log((1, "W", A, 1), (1, "W", B, 2), (1, "W", C, 1), (1, "W", P, 1), (1, "W", X, 1)))
    status, out = diff([base, base], own)
    assert status == 1 and "differ in 2 places" in out, out
    assert "base record 2, own record 2" in out and "base nothing, before record 5, own record 5" in out, out


def test_volatile_from_any_pair(f):
    b1 = f("b1", log((1, "W", A, 1), (1, "R", V, 5)))
    b2 = f("b2", log((1, "W", A, 1), (1, "R", V, 7)))
    assert diff([b1, b2], f("own", log((1, "W", A, 1), (1, "R", V, 9))))[0] == 0
    status, out = diff([b1, b2], f("own2", log((1, "W", A, 2), (1, "R", V, 5))))
    assert status == 1 and "base record 1, own record 1" in out, out


def test_interrupt_thread_is_a_multiset(f):
    role = "trace: thread 2 isr"
    base = f("base", log((1, "W", A, 1), (2, "R", X, 0), (2, "R", Y, 0), role))
    own = f("own", log((2, "R", Y, 0), (1, "W", A, 1), (2, "R", X, 0), role))
    assert diff([base, base], own) == (0, "mac-trace: every thread agrees, by operation, width and address, "
                                          "and by value, 0 volatile words aside\n")
    extra = f("extra", log((1, "W", A, 1), (2, "R", X, 0), (2, "R", Y, 0), (2, "R", Y, 0), role))
    status, out = diff([base, base], extra)
    assert status == 0 and "thread 2 is an interrupt's" in out and "own has x1" in out, out


def test_join_makes_one_stream(f):
    base = f("base", log((1, "W", A, 1), (3, "W", B, 1), (3, "W", C, 1)))
    own = f("own", log((1, "W", A, 1), (1, "W", B, 1), (1, "W", C, 1)))
    assert diff([base, base], own)[0] == 1
    assert diff([base, base], own, join=True)[0] == 0


def test_from_aligns_and_cuts_a_cycle(f):
    # The own run adds an access before the marker, and its cycle starts one access earlier and turns once less;
    # from the marker on, both hold the cycle's last two turns and the write after it.
    base = f("base", log((1, "W", A, 1), ("cycle", 4, 1), (1, "R", P, 0), (1, "W", C, 1),
                         "trace: req 3 t1 mark 0 0"))
    own = f("own", log((1, "W", A, 1), (1, "W", B, 1), ("cycle", 3, 1), (1, "R", P, 0), (1, "W", C, 1),
                       "trace: req 3 t1 mark 0 0"))
    assert diff([base, base], own)[0] == 1
    status, out = diff([base, base], own, from_="mark")
    assert status == 0 and "polling" not in out, out
    acc, _ = mt.scan(base)
    assert mt.by_thread(acc, 3, 1 << 62, None, set()) == {1: [("R", 4, P, 0, 2), ("W", 4, C, 1, 1)]}


def test_short_log_is_refused(f):
    base = f("base", log((1, "W", A, 1), (1, "W", B, 1), (1, "W", C, 1)))
    lines = log((1, "W", A, 1), (1, "W", B, 1), (1, "W", C, 1)).splitlines()
    own = f("own", "\n".join([lines[0], lines[2]]) + "\n")
    try:
        diff([base, base], own)
    except SystemExit as e:
        assert "short" in str(e), e
    else:
        raise AssertionError("a log missing a sequence number passed")


def test_another_build_is_refused(f):
    image = f("drv.bin", "this build")
    here = "# flashed 0x210000 sha256 %s" % hashlib.sha256(b"this build").hexdigest()
    other = "# flashed 0x210000 sha256 %s" % hashlib.sha256(b"the last build").hexdigest()
    base = f("base", log(here, (1, "W", A, 1)))
    assert diff([base, base], f("own", log(here, (1, "W", A, 1))), image=image)[0] == 0
    try:
        diff([base, f("stale", log(other, (1, "W", A, 1)))], f("own2", log(here, (1, "W", A, 1))), image=image)
    except SystemExit as e:
        assert "did not flash" in str(e), e
    else:
        raise AssertionError("a base that flashed another build passed")


def test_compare_volatile_and_counters(f):
    k = next(iter(mt.COUNTERS))
    dry = [f("d1", snaps({X: 1, Y: 5, k: 10})), f("d2", snaps({X: 1, Y: 5, k: 10})), f("d3", snaps({X: 2, Y: 5, k: 10}))]
    status, out = run(mt.compare, dry=dry, traced=f("t", snaps({X: 3, Y: 5, k: 12})))
    assert status == 0 and "1 volatile" in out and "1 counters, not compared" in out, out
    status, out = run(mt.compare, dry=dry, traced=f("t2", snaps({X: 1, Y: 6, k: 10})))
    assert status == 1 and "divergent 0x%08x" % Y in out, out


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
