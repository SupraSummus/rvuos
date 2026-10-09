#!/usr/bin/env python3
"""A step of the ESP32-C6's own bring-up through the checks CLAUDE.md names, on the board, one after another.

  tools/esp32c6-gate.py [--join CONF]... [--probe SSID] [--snap CONF] [--no-check] [--dry-run]

In order:
the snap= list: --snap's, or the one the last gate's traced runs read, or, with none kept, a trace=1 run of each start's;
four trace=1 runs of the libraries' start and one of the own start,
made again with what they read if a kept list lacks a word they all read, and what they read kept for the next gate;
the host replay of macstart.c against the first base's accesses, and whether test/replay holds what the four give;
the diff of the access streams, judged against the places by design in user/wifi/esp32c6/test/diff-expected.txt;
four trace=2 runs of each start, interleaved, and the three compares of the final state:
the own start's traced run against its dry runs, the libraries' against theirs, and across the images;
the scan, listen= and listen= with probe=; a join for each --join; and make check.
Every configuration it writes and every log stays in build/esp32c6/gate/.
A check that could not run is told as not run, and the exit status is 1 when any check failed.
The probe's network is --probe, or the ssid= of the first --join, which is read and never printed.
"""

import argparse
import importlib.util
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BUILD = "build/esp32c6"
DIR = os.path.join(BUILD, "gate")
EXPECTED = "user/wifi/esp32c6/test/diff-expected.txt"  # the diff's places by design, which the diff rule reads
COMPARE_EXPECTED = "user/wifi/esp32c6/test/compare-expected.txt"  # the final state's words by design, across images
REPLAY = "user/wifi/esp32c6/test/replay"  # the libraries' accesses mac-replay-test holds macstart.c to
spec = importlib.util.spec_from_file_location("mac_trace", os.path.join(ROOT, "tools", "mac-trace.py"))
mt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mt)

# Each make the gate runs starts afresh, not as a part of the make that may have started the gate:
# without its flags, and without the variables its command line set, which make puts in the environment,
# BOARD=esp32c6 among them, so that make check builds for its own boards.
ENV = {k: v for k, v in os.environ.items()
       if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")
       and k not in re.findall(r"(?:^|\s)([A-Za-z_][A-Za-z0-9_]*)=", os.environ.get("MAKEFLAGS", ""))}

# The seconds a run on the board gets: a traced one takes about thirty, the rest less.
# One that lost its halt line, or whose board stopped short of it, would wait for it for good, so it is stopped then,
# and the status is timeout(1)'s.
BOARD_SECONDS = 180
HUNG = 124

# The chip's temperature, at the last reading the PHY library took in a traced run: the sensor's code, TSENS_OUT,
# and its range, which the library reads back over the analog I2C, block 0x69's register 6; esp-idf's
# temperature_sensor_attributes gives each range's offset, and temperature_sensor_ll.h the line through them.
# The eFuse's calibration is not read, so it is the sensor's own, within its range's error, 2 C at 20 to 100.
TSENS = re.compile(r"R4 0x6000e058 = 0x[0-9a-f]{6}([0-9a-f]{2})\b")
TSENS_RANGE = re.compile(r"R4 0x600af800 = 0x00([0-9a-f]{2})0669\b")
TSENS_OFFSET = {5: -2, 7: -1, 15: 0, 11: 1, 10: 2}

SNAPLINE = re.compile(r"^snap=([0-9a-f]+)(?:-([0-9a-f]+))?$", re.M)
# The snap= lines the last gate's traced runs read, which the next takes rather than two runs of its own.
# What a run reads varies with what the air brings over its window, so the lines are the union of the five runs',
# and a gate that takes them holds them to the words every one of its own five read: a word the start reads that they
# lack makes the runs go again, with the lines their reads give.
SNAPS = os.path.join(DIR, "snap.list")
# What a run's log says of the checks the gate reports beside its verdict.
SAID = re.compile(r"listen: in .*|probe: \d+ sent.*|every check passed.*|\S+: failed.*")


class Gate:
    def __init__(self, args):
        self.args = args
        self.rows = []  # (check, verdict, detail)

    def run(self, name, cmd, seconds=None):
        """A command's exit status, its output in DIR/name.out; with --dry-run, the command alone, as passed.
        One still running after seconds is stopped, the make and every process it started, and its status is HUNG;
        one running when the gate is interrupted is stopped too, since its own session keeps the terminal's ^C from it.
        """
        if self.args.dry_run:
            print("  " + " ".join(cmd))
            return 0
        with open(os.path.join(DIR, name + ".out"), "w") as out:
            p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=ROOT, env=ENV, start_new_session=True)
            try:
                return p.wait(seconds)
            except subprocess.TimeoutExpired:
                out.write("gate: stopped after %d s\n" % seconds)
                return HUNG
            finally:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGTERM)
                    p.wait()

    def board(self, name, conf):
        """A run on the board with the configuration at conf, its log kept as DIR/name.log.
        The last run's log goes first, so that a run that wrote none leaves none rather than another's.
        """
        log = os.path.join(BUILD, "wifi-run.log")
        for path in (log, os.path.join(DIR, name + ".log")):
            if os.path.exists(path) and not self.args.dry_run:
                os.remove(path)
        status = self.run(name, ["make", "BOARD=esp32c6", "wifi-esp32c6", "WIFI_CONFIG=" + conf], BOARD_SECONDS)
        if os.path.exists(log) and not self.args.dry_run:
            shutil.copy(log, os.path.join(DIR, name + ".log"))
        return status

    def traced(self, name, trace, lib, snap):
        """A traced run, made again, twice at most, when the stream lost bytes of its log (see TODO.md)
        or the run never halted, each loss written down in DIR/losses.txt with the run, the place and the line,
        and the run's log kept beside it, DIR/name-lost-TIME.log, for the cause to be read from.
        """
        conf = os.path.join(DIR, name + ".conf")
        with open(conf, "w") as f:
            f.write("trace=%d\nrun=60\n%s%s" % (trace, "libstart=1\n" if lib else "", snap))
        for attempt in range(3):
            status = self.board(name, conf)
            if status == HUNG:
                lost = "no halt within %d s" % BOARD_SECONDS
            else:
                lost = None if status != 0 or self.args.dry_run else loss(os.path.join(DIR, name + ".log"), trace, snap)
            if not lost:
                return status
            kept = os.path.join(DIR, "%s-lost-%s.log" % (name, time.strftime("%Y%m%d-%H%M%S")))
            if os.path.exists(os.path.join(DIR, name + ".log")):
                shutil.copy(os.path.join(DIR, name + ".log"), kept)
            warm = self.chip(name)
            with open(os.path.join(DIR, "losses.txt"), "a") as f:
                f.write("%s %s: %s; %s%s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), name, lost, kept,
                                               "; " + warm if warm else ""))
            print("gate: %s: %s; %s" % (name, lost, "run again" if attempt < 2 else "given up"), flush=True)
        return 1

    def chip(self, name):
        """What a traced run's log says of the chip's temperature, beside its verdict."""
        c = None if self.args.dry_run else chip(os.path.join(DIR, name + ".log"))
        return "" if c is None else "chip %.0f C" % c

    def said(self, name):
        if self.args.dry_run:
            return ""
        with open(os.path.join(DIR, name + ".out"), errors="replace") as f:
            found = SAID.findall(f.read())
        return re.sub(r"([0-9a-f]{2}:){5}[0-9a-f]{2}", "the AP", found[-1]) if found else ""

    def tell(self, check, status, detail=""):
        verdict = "ok" if status == 0 else "not run" if status is None else "FAIL (%d)" % status
        self.rows.append((check, verdict, detail))
        print("gate: %-28s %s%s" % (check, verdict, "  " + detail if detail else ""), flush=True)


def chip(log):
    """The chip's temperature in C at the last reading a traced run's log holds, or None."""
    if not os.path.exists(log):
        return None
    text = open(log, errors="replace").read()
    codes, ranges = TSENS.findall(text), TSENS_RANGE.findall(text)
    offset = TSENS_OFFSET.get(int(ranges[-1], 16) & 0xF) if ranges else None
    if not codes or offset is None:
        return None
    return (4386 * int(codes[-1], 16) - 278800 * offset - 205200) / 10000


def loss(log, trace, snap):
    """Where the stream lost bytes of a traced run's log, or None:
    a sequence number missing or a line cut, a phase marker whose front went, or a snapshot short of its lines.
    """
    acc, damaged = mt.scan(log)
    if damaged:
        return "seq %d %s" % damaged[0]
    if trace == 1:
        for name in ("start", "stop"):
            if mt.phase(log, name) is None:
                return "the %s marker %s" % (name, "cut: " + mt.cut_marker(log, name) if mt.cut_marker(log, name)
                                             else "missing")
    if snap:
        try:
            mt.snapshots(log)
        except SystemExit as e:
            return str(e)
    return None


def words(text):
    """The words snap= lines name."""
    found = set()
    for lo, hi in SNAPLINE.findall(text):
        found.update(range(int(lo, 16), int(hi or lo, 16) + 4, 4))
    return found


def lines(found):
    """The snap= lines that name the words found."""
    return "".join("snap=%x\n" % lo if lo == hi else "snap=%x-%x\n" % (lo, hi) for lo, hi in mt.ranges(found))


def read(name):
    """The words a traced run read, as mac-trace.py snapshot gives them."""
    return words(subprocess.run([sys.executable, "tools/mac-trace.py", "snapshot", os.path.join(DIR, name + ".log")],
                                capture_output=True, text=True, cwd=ROOT).stdout)


def snap_list(gate):
    """The snap= lines and whether they are the last gate's:
    those of --snap's configuration; or what the last gate's traced runs read, kept in SNAPS and held below to what
    this gate's read; or, with none kept, the union of what a traced run of each start read.
    """
    kept = not gate.args.snap and os.path.exists(SNAPS)
    if gate.args.snap or kept:
        found = words(open(gate.args.snap or SNAPS).read())
    else:
        found = set()
        for name, lib in (("snap-lib", True), ("snap-own", False)):
            if gate.traced(name, 1, lib, "") != 0:
                sys.exit("gate: the traced run %s failed; see %s" % (name, os.path.join(DIR, name + ".out")))
            if not gate.args.dry_run:
                found |= read(name)
    if not found and not gate.args.dry_run:
        sys.exit("gate: no snap= lines")
    return lines(found), kept


def traced_runs(gate, snap):
    """The four trace=1 runs of the libraries' start and the one of the own start, each verdict told;
    the words each read, or None when one failed.
    """
    names = ["base%d" % i for i in range(1, 5)] + ["own"]
    failed = False
    for name in names:
        status = gate.traced(name, 1, name != "own", snap)
        gate.tell("trace=1 " + name.replace("base", "base "), status, gate.chip(name))
        failed = failed or status != 0
    return None if failed or gate.args.dry_run else [read(name) for name in names]


def host_replay(gate):
    """The bring-up's sequences replayed on the host against the libraries' accesses of the first base,
    taken into DIR/replay;
    and whether test/replay holds what the four bases give, so that a group taken over is not held to files taken
    before it: each access as they all give it, and where they differ in a value, the moment's, its operation and address.
    """
    base1 = os.path.join(DIR, "base1.log")
    taken = [os.path.join(DIR, "replay")] + [os.path.join(DIR, "replay-%d" % i) for i in range(2, 5)]
    if not gate.args.dry_run:
        for d in taken:
            shutil.rmtree(d, ignore_errors=True)  # a file an earlier gate took, and this one no longer does, is not held
    status = gate.run("replay", ["make", "BOARD=esp32c6", "wifi-esp32c6-replay", "WIFI_TRACE_LOG=" + base1,
                                 "MAC_REPLAY_DIR=" + taken[0]])
    detail = ""
    if status != 0:
        detail = "the files could not be taken; see %s" % os.path.join(DIR, "replay.out")
    else:
        status = gate.run("replay-test", ["make", "mac-replay-test", "MAC_REPLAY_DIR=" + taken[0]])
        if not gate.args.dry_run:
            said = re.findall(r"^mac-replay-test: .*", open(os.path.join(DIR, "replay-test.out")).read(), re.M)
            detail = said[0] if said else ""
    gate.tell("host replay", status, detail)
    if gate.args.dry_run or status != 0:
        return
    for i, d in enumerate(taken[1:], 2):
        if gate.run("replay-%d" % i, ["make", "BOARD=esp32c6", "wifi-esp32c6-replay",
                                      "WIFI_TRACE_LOG=" + os.path.join(DIR, "base%d.log" % i), "MAC_REPLAY_DIR=" + d]):
            gate.tell("test/replay as bases give", 1, "see %s" % os.path.join(DIR, "replay-%d.out" % i))
            return

    def accesses(path):
        return [line.split() for line in open(path) if not line.startswith("#")] if os.path.exists(path) else None

    # A file test/replay holds that the bases no longer give is stale too, a case the test may still read.
    stale, moments = [], 0
    for f in sorted(set(os.listdir(REPLAY)).union(*(os.listdir(d) for d in taken))):
        held = accesses(os.path.join(REPLAY, f))
        given = [accesses(os.path.join(d, f)) for d in taken]
        if held is None or None in given or any(len(g) != len(held) for g in given):
            stale.append(f)
            continue
        for line, *bases in zip(held, *given):
            if all(b == bases[0] for b in bases):
                wrong = line != bases[0]
            else:
                moments += 1
                wrong = any(b[:2] != line[:2] for b in bases)
            if wrong:
                stale.append(f)
                break
    gate.tell("test/replay as bases give", 1 if stale else 0,
              "%s differ: make BOARD=esp32c6 wifi-esp32c6-replay WIFI_TRACE_LOG=%s" % (", ".join(stale), base1)
              if stale else "%d values the bases differ in, held by address" % moments if moments else "")


def probe_ssid(args):
    if args.probe:
        return args.probe
    for conf in args.join[:1]:
        m = re.search(r"^ssid=(.*)$", open(conf).read(), re.M)
        if m:
            return m.group(1).strip()
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--join", action="append", default=[], metavar="CONF",
                   help="a network to join, from a file git does not track; repeatable")
    p.add_argument("--probe", metavar="SSID", help="the network the listen run probes; else the first --join's")
    p.add_argument("--snap", metavar="CONF", help="take the snap= lines of this configuration, rather than two runs'")
    p.add_argument("--no-check", action="store_true", help="leave make check out, told as not run")
    p.add_argument("--dry-run", action="store_true", help="print the commands, run nothing on the board")
    args = p.parse_args()
    os.makedirs(os.path.join(ROOT, DIR), exist_ok=True)
    os.chdir(ROOT)
    gate = Gate(args)

    if gate.run("build", ["make", "BOARD=esp32c6", BUILD + "/wifi-drv.bin"]) != 0:
        sys.exit("gate: the driver did not build; see %s" % os.path.join(DIR, "build.out"))
    snap, kept = snap_list(gate)
    reads = traced_runs(gate, snap)
    missed = set.intersection(*reads) - words(snap) if reads and kept else None
    if missed:
        print("gate: the kept snap= lines lack %d words every traced run read, %s; the traced runs again"
              % (len(missed), lines(missed).replace("\n", " ").strip()), flush=True)
        del gate.rows[-5:]
        snap = lines(set.union(*reads))
        reads = traced_runs(gate, snap)
    if reads:
        open(SNAPS, "w").write(lines(set.union(*reads)))
    host_replay(gate)
    bases = " ".join(os.path.join(DIR, "base%d.log" % i) for i in range(1, 5))
    status = gate.run("diff", ["make", "BOARD=esp32c6", "wifi-esp32c6-diff", "WIFI_TRACE_BASE=" + bases,
                               "WIFI_TRACE_OWN=" + os.path.join(DIR, "own.log")])
    places = ""
    if not args.dry_run:
        text = open(os.path.join(DIR, "diff.out"), errors="replace").read()
        found = re.findall(r"they differ in (\d+) place", text)
        met = len(re.findall(r"^  t\d+ expected, ", text, re.M))
        places = ("differs in %s places, see %s" % ("+".join(found), os.path.join(DIR, "diff.out")) if found
                  else "%d expected places" % met if status == 0 and met else "" if status == 0
                  else "see %s" % os.path.join(DIR, "diff.out"))
    if os.path.exists(EXPECTED):
        gate.tell("diff, four bases", status, places)
    else:
        print("gate: %-28s %s" % ("diff, four bases", places + ", to read: no " + EXPECTED), flush=True)
        gate.rows.append(("diff, four bases", "read", places))

    for i in range(1, 5):
        gate.tell("trace=2 libraries' %d" % i, gate.traced("dry-lib%d" % i, 2, True, snap))
        gate.tell("trace=2 own %d" % i, gate.traced("dry-own%d" % i, 2, False, snap))
    lib_dry = [os.path.join(DIR, "dry-lib%d.log" % i) for i in range(1, 5)]
    own_dry = [os.path.join(DIR, "dry-own%d.log" % i) for i in range(1, 5)]
    compare = [sys.executable, "tools/mac-trace.py", "compare"]
    gate.tell("compare, own start", gate.run("compare-own", compare + own_dry + [os.path.join(DIR, "own.log")]))
    gate.tell("compare, libraries' start",
              gate.run("compare-lib", compare + lib_dry + [os.path.join(DIR, "base1.log")]))
    across = ["--expect", COMPARE_EXPECTED] if os.path.exists(COMPARE_EXPECTED) else []
    gate.tell("compare, across images", gate.run("compare-across", compare + across + lib_dry + own_dry[:1]))

    for name, text in (("scan", ""), ("listen", "listen=1\nrun=15\n")):
        conf = os.path.join(DIR, name + ".conf")
        open(conf, "w").write(text)
        gate.tell(name, gate.board(name, conf), gate.said(name))
    ssid = probe_ssid(args)
    if ssid:
        conf = os.path.join(DIR, "probe.conf")
        open(conf, "w").write("listen=1\nprobe=%s\nrun=15\n" % ssid)
        gate.tell("listen and probe", gate.board("probe", conf), gate.said("probe"))
    else:
        gate.tell("listen and probe", None, "no --probe and no --join")
    for i, conf in enumerate(args.join, 1):
        gate.tell("join %s" % os.path.basename(conf), gate.board("join%d" % i, conf), gate.said("join%d" % i))
    if not args.join:
        gate.tell("joins", None, "no --join")

    if args.no_check:
        gate.tell("make check", None, "--no-check")
    else:
        status = gate.run("check", ["make", "check"])
        fails = 0 if args.dry_run else open(os.path.join(DIR, "check.out"), errors="replace").read().count("FAIL")
        gate.tell("make check", status if not fails else status or 1, "%d FAIL lines" % fails if fails else "")

    failed = [r for r in gate.rows if r[1].startswith("FAIL")]
    print("gate: %d checks, %d failed, %d not run; logs in %s"
          % (len(gate.rows), len(failed), sum(r[1] == "not run" for r in gate.rows), DIR))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
