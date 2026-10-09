#!/usr/bin/env python3
"""The text logic of tools/mac-trace.py, on synthetic logs of a few lines each.

The tool is the oracle of every step of the ESP32-C6's own bring-up,
so what it lets pass or holds back has to be what its help says:
a polling loop's turns are a note, not a difference;
a difference does not hide what comes after it;
a word any pair of base or dry runs differs in is volatile, even where two of them agree;
an interrupt thread's accesses are a multiset;
--join makes one stream of every thread but the interrupt's;
--from aligns two runs by their phase markers, a cycle cut at the marker keeping its share;
a short log is refused, and with --image one that flashed another build, and a marker cut by a loss is told as one;
diff's expectations let a place the own start differs in by design pass, that place alone and exactly;
the bits MOMENT names stay out of compare's and diff's verdict, the rest of their word in;
and replay takes the named functions' own accesses alone, between the functions --from and --to name,
or up to the last access of the one --through names, and none within a function --outside names.
The ELF and the libraries are not read: every pc is given to one library, and to the function FUNCTIONS names.
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
PC = 0x42000010
FUNCTIONS = {PC: "f", PC + 4: "g", PC + 8: "h"}
mt.classifier = lambda args, pcs: (lambda pc: "lib", FUNCTIONS)


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
            pc = item[4] if len(item) > 4 else PC
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


def diff(bases, own, **kw):
    args = dict(base=bases, own_log=own, from_=None, to=None, join=False, collapse=[], elf="", rom="", lib=[], own=[],
                image=None, expect=None, as_expected=False)
    args.update(kw)
    return run(mt.diff, **args)


def compare(dry, traced, expect=None):
    return run(mt.compare, dry=dry, traced=traced, expect=expect)


def replay(trace, *function, **kw):
    args = dict(log=trace, function=list(function), from_=None, to=None, through=None, outside=[], image=None)
    args.update(kw)
    return run(mt.replay, **args)


A, B, C, P, V, X, Y = (0x600A4000 + 4 * i for i in range(7))
TX_BLOCK = 0x600A4CA8


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


def test_an_expected_place_is_met_exactly(f):
    # The own start writes B its own way, by design: an expectation naming that place, reason and all, lets it pass;
    # the same place with another value fails, and so does an expectation no place meets.
    base = f("base", log((1, "W", A, 1), (1, "R", B, 0), (1, "W", B, 4), (1, "W", C, 1)))
    own = f("own", log((1, "W", A, 1), (1, "W", B, 5), (1, "W", C, 1)))
    place = "# B written once\n-R4 0x%08x 0x00000000\n-W4 0x%08x 0x00000004\n+W4 0x%08x 0x00000005\n" % (B, B, B)
    status, out = diff([base, base], own, expect=f("expect", place))
    assert status == 0 and "expected, base records 2-3, own record 2: B written once" in out, out
    status, out = diff([base, base], f("own2", log((1, "W", A, 1), (1, "W", B, 6), (1, "W", C, 1))),
                       expect=f("expect2", place))
    assert status == 1 and "differ in 1 place" in out and "did not come: B written once" in out, out
    masked = place.replace("+W4 0x%08x 0x00000005" % B, "+W4 0x%08x 0x00000000/0xfffffff0" % B)
    assert diff([base, base], own, expect=f("masked", masked))[0] == 0
    status, out = diff([base, base], base, expect=f("expect3", place))
    assert status == 1 and "did not come" in out, out
    twice = f("twice", log((1, "R", B, 0), (1, "W", B, 4), (1, "W", C, 1), (1, "R", B, 0), (1, "W", B, 4)))
    status, out = diff([twice, twice], f("own3", log((1, "W", B, 5), (1, "W", C, 1), (1, "W", B, 5))),
                       expect=f("expect4", place))
    assert status == 1 and "differ in 1 place" in out, out


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


def test_join_keeps_the_interrupt_apart(f):
    # The interrupt came at another moment and once more: with --join the bring-up's threads are one stream,
    # and the interrupt's is still a multiset, reported apart.
    role = "trace: thread 2 isr"
    base = f("base", log((1, "W", A, 1), (2, "R", X, 0), (3, "W", B, 1), role))
    own = f("own", log((1, "W", A, 1), (1, "W", B, 1), (2, "R", X, 0), (2, "R", X, 0), role))
    status, out = diff([base, base], own, join=True)
    assert status == 0 and "thread 2 is an interrupt's" in out and "own has x1" in out, out


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


def test_a_cut_marker_is_told(f):
    # The stream lost the front of the line naming the start: diff says so, not that the driver never named it.
    base = f("base", log((1, "W", A, 1), "   1.000 trace: req 1 t1 start 0 0"))
    own = f("own", log((1, "W", A, 1), "   1.000 1 t1 start 0 0"))
    try:
        diff([base, base], own, from_="start")
    except SystemExit as e:
        assert "lost the front" in str(e) and "1 t1 start 0 0" in str(e), e
    else:
        raise AssertionError("a log whose marker lost its front passed")


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


def test_compare_volatile_and_the_moment(f):
    k = next(a for a, (mask, _) in mt.MOMENT.items() if mask == 0xFFFFFFFF)
    dry = [f("d1", snaps({X: 1, Y: 5, k: 10})), f("d2", snaps({X: 1, Y: 5, k: 10})), f("d3", snaps({X: 2, Y: 5, k: 10}))]
    status, out = compare(dry, f("t", snaps({X: 3, Y: 5, k: 12})))
    assert status == 0 and "1 volatile" in out and "1 of the moment, not compared" in out, out
    status, out = compare(dry, f("t2", snaps({X: 1, Y: 6, k: 10})))
    assert status == 1 and "divergent 0x%08x" % Y in out, out


def test_the_moments_bits_alone_are_read_apart(f):
    # TX_BLOCK's busy bits are the moment's, its other bits the code's: in compare and in diff alike.
    t, busy = TX_BLOCK, mt.MOMENT[TX_BLOCK][0]
    dry = [f("d%d" % i, snaps({t: 0x1000})) for i in range(2)]
    assert compare(dry, f("t", snaps({t: 0x1000 | busy})))[0] == 0
    assert compare(dry, f("t2", snaps({t: 0})))[0] == 1
    base = f("base", log((1, "R", t, 0), (1, "W", t, 0)))
    assert diff([base, base], f("own", log((1, "R", t, busy), (1, "W", t, busy))))[0] == 0
    assert diff([base, base], f("own2", log((1, "R", t, 0), (1, "W", t, 0x1000))))[0] == 1


def test_an_expected_word_passes_exactly(f):
    # The own start leaves a bit its final state would hold out by design: that difference alone passes.
    dry = [f("d%d" % i, snaps({X: 0x41, Y: 5})) for i in range(2)]
    expect = f("expect", "# Interface 1's bit, which the own start leaves out.\n0x%08x 0x00/0x40\n" % X)
    status, out = compare(dry, f("t", snaps({X: 0x01, Y: 5})), expect=expect)
    assert status == 0 and "by design 0x%08x" % X in out, out
    for name, words in (("outside", {X: 0x00, Y: 5}), ("as dry", {X: 0x41, Y: 5}), ("other", {X: 0x01, Y: 6})):
        status, out = compare(dry, f(name, snaps(words)), expect=expect)
        assert status == 1, (name, out)
    # Dry runs that leave the bit out as well: the expected difference is gone, and the file is stale.
    gone = [f("g%d" % i, snaps({X: 0x01, Y: 5})) for i in range(2)]
    status, out = compare(gone, f("t2", snaps({X: 0x01, Y: 5})), expect=expect)
    assert status == 1 and "not as expected 0x%08x" % X in out, out
    # A word the dry runs differ in cannot be held to a value either.
    varies = [f("v1", snaps({X: 0x41, Y: 5})), f("v2", snaps({X: 0x43, Y: 5}))]
    assert compare(varies, f("t3", snaps({X: 0x01, Y: 5})), expect=expect)[0] == 1


def test_replay_takes_one_function(f):
    # f writes A around g, which reads B; h calls f again later: --to h keeps the first call alone.
    g, h = PC + 4, PC + 8
    trace = f("lib", log("# flashed 0x210000 sha256 " + "ab" * 32,
                         (1, "R", A, 1), (1, "W", A, 3), (1, "R", B, 7, g), (1, "W", A, 2),
                         (1, "W", C, 0, h), (1, "W", A, 9)))
    status, out = replay(trace, "f", to="h")
    assert status == 0, out
    assert out.splitlines() == ["# f up to h, in a run of the image sha256 " + "ab" * 32,
                                "R4 0x%08x 0x00000001" % A, "W4 0x%08x 0x00000003" % A,
                                "W4 0x%08x 0x00000002" % A], out
    status, out = replay(trace, "f", "h", from_="h")
    assert out.splitlines()[1:] == ["W4 0x%08x 0x00000000" % C, "W4 0x%08x 0x00000009" % A], out
    # g, which the own code still calls, calls f in its turn: --outside g leaves f's call within g out.
    nested = f("nested", log((1, "W", A, 1), (1, "R", B, 7, g), (1, "W", C, 2), (1, "W", B, 8, g), (1, "W", A, 3)))
    status, out = replay(nested, "f", outside=["g"])
    assert out.splitlines()[1:] == ["W4 0x%08x 0x00000001" % A, "W4 0x%08x 0x00000003" % A], out
    # h ran before f too, so --to h would take nothing; --through g stops after g's last access, before f's next call.
    early = f("early", log((1, "W", C, 0, h), (1, "R", A, 1), (1, "R", B, 7, g), (1, "W", A, 2), (1, "W", B, 8, g),
                           (1, "W", C, 5, h), (1, "W", A, 9)))
    status, out = replay(early, "f", "g", through="g")
    assert out.splitlines() == ["# f g through g, in a run of the image sha256 unknown",
                                "R4 0x%08x 0x00000001" % A, "R4 0x%08x 0x00000007" % B,
                                "W4 0x%08x 0x00000002" % A, "W4 0x%08x 0x00000008" % B], out


def test_replay_refuses(f):
    for name, text, why in [("cycle", log((1, "W", A, 1), ("cycle", 3, 1), (1, "R", P, 0)), "cycle"),
                            ("none", log((1, "W", A, 1, PC + 4)), "no access of f"),
                            ("short", "\n".join(log((1, "W", A, 1), (1, "W", B, 1), (1, "W", C, 1))
                                                 .splitlines()[::2]) + "\n", "short")]:
        try:
            replay(f(name, text), "f")
        except SystemExit as e:
            assert why in str(e), e
        else:
            raise AssertionError("replay took the %s log" % name)


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
