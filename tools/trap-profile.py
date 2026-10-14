#!/usr/bin/env python3
"""Boot an image under QEMU and say what each kind of trap cost the kernel, how often it came, and where it went.

QEMU logs every instruction of the kernel's it runs, each translated alone,
since under -icount a longer block that reaches a device is left halfway and run again, and would count twice.
The log streams through a pipe, never written whole.
Under -icount shift=0 a run repeats exactly, so a change shows as the instructions it moved, function by function.
The counts are instructions, not cycles; on silicon a CSR or a load costs what the core says.

A trap runs from the first instruction of trap_entry, or of RISC-V's vector before it,
to the return at the end of trap_return, mret or bx lr.
Its class is what it ran:
"call" and the op_ function the call reached, with the wait or signal of the scheduler it went through,
"irq" for a device's interrupt, "fault", and "timer" for any other interrupt;
then "+switch" if it gave the core to another thread, "+regions" if it loaded another process's,
and "+idle" if the core idled before it left.
What an idle core spends waiting in intr_wait is left out of every count:
it is time nobody wanted, not the trap's cost.

The table gives each class's traps, their mean, least and most instructions,
and the class's share of all the kernel ran, the cost times how often it came;
then for the heaviest classes the mean each function took, a function's inlined callees counted as its own.
--save writes the profile as JSON, and --against holds the run to a saved one,
saying how each class's mean and each of its functions moved.

--annotate breaks one function down by the source lines it ran, its inlined callees' lines among them.

Usage: trap-profile.py [--nm tool] [--objdump tool] [--symbolizer tool] [--top N] [--save file] [--against file]
                       [--annotate function] kernel.elf boot...
where boot is the QEMU command that boots the image, to which the logging flags are appended.
"""

import argparse
import bisect
import collections
import json
import os
import subprocess
import sys
import tempfile
import threading


# What the kernel's own functions say of the trap that ran them.
MARKS = (("sched_wait", " wait"), ("sched_signal", " signal"))
FLAGS = (("switch_to", " +switch"), ("process_activate", " +regions"), ("core_idle", " +idle"))
IDLE_WAIT = "intr_wait"


def text_of(objdump, elf):
    """The kernel's .text as its address and size, and whether its code is Thumb."""
    out = subprocess.run([objdump, "-h", elf], capture_output=True, text=True, check=True).stdout
    thumb = "littlearm" in out
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == ".text":
            return int(parts[3], 16), int(parts[2], 16), thumb
    sys.exit(f"trap-profile: no .text in {elf}")


def symbols(nm, elf, thumb):
    """The image's functions as sorted starts and names, a Thumb function's start without its bit."""
    out = subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT":
            found[int(parts[0], 16) & ~(1 if thumb else 0)] = parts[2]
    starts = sorted(found)
    return starts, [found[s] for s in starts]


def return_pc(objdump, elf, thumb):
    """The address of trap_return's return to the thread."""
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--disassemble-symbols=trap_return", elf],
                         capture_output=True, text=True, check=True).stdout
    want = "bx\tlr" if thumb else "mret"
    for line in out.splitlines():
        if want in line:
            return int(line.split(":")[0], 16)
    sys.exit(f"trap-profile: no {want} in trap_return")


def classify(funcs):
    if "syscall_dispatch" in funcs:
        ops = [f for f in funcs if f.startswith("op_")]
        kind = "call " + (ops[0] if ops else "?")
        kind += "".join(mark for f, mark in MARKS if f in funcs)
    elif "fault_dispatch" in funcs:
        kind = "fault"
    elif "sched_claim_interrupts" in funcs:
        kind = "irq"
    else:
        kind = "timer"
    return kind + "".join(flag for f, flag in FLAGS if f in funcs)


def executed(log, starts, names, entries, ret):
    """The instructions the log says ran, each as its function, whether it returns or enters, and its address."""
    seen = {}  # the host's address of a translated instruction: what it yields
    for line in log:
        if not line.startswith("Trace"):
            continue
        _, _, host, rest = line.split(" ", 3)
        insn = seen.get(host)
        if insn is None:
            pc = int(rest.split("/", 2)[1], 16)
            insn = seen[host] = (names[bisect.bisect_right(starts, pc) - 1], pc == ret, pc in entries, pc)
        yield insn


def profile(insns):
    """Every trap of the instructions that ran, as {class: {"n", "sum", "min", "max", "funcs"}},
    and how often a trap ran each instruction, by its address."""
    classes = {}
    ran = collections.Counter()
    trap = None
    for func, returns, enters, pc in insns:
        if trap is None:
            if not enters:
                continue
            trap = collections.Counter()
        trap[func] += 1
        ran[pc] += 1
        if returns:
            kind = classify(trap)
            trap.pop(IDLE_WAIT, None)
            total = sum(trap.values())
            c = classes.setdefault(kind, {"n": 0, "sum": 0, "min": total, "max": total, "funcs": collections.Counter()})
            c["n"] += 1
            c["sum"] += total
            c["min"] = min(c["min"], total)
            c["max"] = max(c["max"], total)
            c["funcs"].update(trap)
            trap = None
    return classes, ran


def report(classes, top, against):
    grand = sum(c["sum"] for c in classes.values()) or 1
    traps = sum(c["n"] for c in classes.values())
    print(f"trap-profile: {traps} traps, {grand} kernel instructions, an idle core's wait left out")
    print(f"  {'class':50} {'traps':>8} {'mean':>6} {'least':>6} {'most':>6} {'share':>6}"
          + ("   was mean" if against else ""))
    order = sorted(classes.items(), key=lambda kv: -kv[1]["sum"])
    for kind, c in order:
        line = (f"  {kind:50} {c['n']:8} {c['sum'] // c['n']:6} {c['min']:6} {c['max']:6}"
                f" {100 * c['sum'] / grand:5.1f}%")
        if against:
            was = against.get(kind)
            if was:
                before = was["sum"] // was["n"]
                line += f"   {before:8} {c['sum'] // c['n'] - before:+6}"
            else:
                line += "   new"
        print(line)
    for kind in (against or {}):
        if kind not in classes:
            print(f"  {kind:50} gone")
    for kind, c in order[:top]:
        print(f"\n{kind}: a trap's mean, by function")
        was = (against or {}).get(kind)
        funcs = collections.Counter(c["funcs"])
        if was:
            funcs.update({f: 0 for f in was["funcs"]})
        for func, v in sorted(funcs.items(), key=lambda kv: -kv[1]):
            mean = v / c["n"]
            line = f"  {func:30} {mean:7.1f}"
            if was:
                before = was["funcs"].get(func, 0) / was["n"]
                if abs(mean - before) < 0.05 and mean < 1:
                    continue
                line += f"   {before:7.1f} {mean - before:+7.1f}"
            elif mean < 1:
                continue
            print(line)


def annotate(symbolizer, elf, func, classes, ran, starts, names):
    """The instructions func ran, by the source line each came from, a trap's mean over the traps that ran it."""
    traps = sum(c["n"] for c in classes.values() if func in c["funcs"])
    if traps == 0:
        sys.exit(f"trap-profile: no trap ran {func}")
    counts = {pc: n for pc, n in ran.items() if names[bisect.bisect_right(starts, pc) - 1] == func}
    addrs = sorted(counts)
    out = subprocess.run([symbolizer, f"--obj={elf}", "--inlines", "--relativenames"],
                         input="".join(f"{a:#x}\n" for a in addrs), capture_output=True, text=True, check=True).stdout
    lines = collections.Counter()
    for addr, frames in zip(addrs, out.strip().split("\n\n")):
        # The innermost frame names the line; the function it lies in, if inlined, stands before it.
        parts = frames.split("\n")
        where = parts[1].rsplit(":", 1)[0] if len(parts) > 1 else "?"
        lines[(where, parts[0])] += counts[addr]
    print(f"\n{func}: a trap's mean by source line, over the {traps} traps that ran it")
    for (where, inner), n in sorted(lines.items(), key=lambda kv: -kv[1]):
        if n / traps >= 0.5:
            print(f"  {n / traps:7.1f}  {where:36} {inner}")


def unblock(qemu, fifo):
    """Once QEMU is gone, opens the pipe for it, so that a QEMU that never opened it leaves no reader waiting."""
    qemu.wait()
    try:
        os.close(os.open(fifo, os.O_WRONLY | os.O_NONBLOCK))
    except OSError:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--nm", default="llvm-nm")
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument("--top", type=int, default=6, help="how many of the heaviest classes to break down")
    parser.add_argument("--save", help="write the profile as JSON")
    parser.add_argument("--against", help="a profile --save wrote, to hold the run to")
    parser.add_argument("--annotate", metavar="function", help="break a function down by the source lines it ran")
    parser.add_argument("--symbolizer", default="llvm-symbolizer")
    parser.add_argument("kernel", help="the image, whose kernel's symbols name the functions")
    parser.add_argument("boot", nargs=argparse.REMAINDER, help="the QEMU command that boots it")
    args = parser.parse_args()

    addr, size, thumb = text_of(args.objdump, args.kernel)
    starts, names = symbols(args.nm, args.kernel, thumb)
    # ARM's table is words, which nothing runs, so only RISC-V's vector is entered by.
    entries = {starts[names.index(n)] for n in ("trap_entry", "trap_vectors") if n in names}
    ret = return_pc(args.objdump, args.kernel, thumb)
    against = None
    if args.against:
        with open(args.against) as f:
            against = json.load(f)

    with tempfile.TemporaryDirectory() as tmp:
        fifo = os.path.join(tmp, "log")
        os.mkfifo(fifo)
        boot = " ".join(args.boot) + f" -accel tcg,one-insn-per-tb=on -d exec,nochain -dfilter {addr:#x}+{size:#x} -D {fifo}"
        errors = open(os.path.join(tmp, "stderr"), "w+")
        qemu = subprocess.Popen(boot, shell=True, stdout=subprocess.DEVNULL, stderr=errors)
        threading.Thread(target=unblock, args=(qemu, fifo), daemon=True).start()
        with open(fifo, errors="replace") as log:
            classes, ran = profile(executed(log, starts, names, entries, ret))
        qemu.wait()
        errors.seek(0)
        error = errors.read()
        errors.close()
    if not classes:
        sys.exit(f"trap-profile: the log held no trap, and QEMU exited with {qemu.returncode}\n{error}")

    report(classes, args.top, against)
    if args.annotate:
        annotate(args.symbolizer, args.kernel, args.annotate, classes, ran, starts, names)
    if args.save:
        with open(args.save, "w") as f:
            json.dump(classes, f, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
