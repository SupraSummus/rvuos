#!/usr/bin/env python3
"""Replay fuzz inputs on QEMU and on the host build and compare the traces.

Each input is a sequence of 16-byte records:
a system call with the thread that makes it, or the tick as OP_DEBUG_TICK.
The host build performs them directly on the kernel's logic,
passing a record to the thread it names as the driver does;
QEMU boots the real kernel with the replay driver as root task
and the input placed in RAM by QEMU's generic loader.
The kernel has no console: on QEMU the replay driver carries the log
to the UART after each record and the halt writes out what the last one left;
on the host each byte is printed as the kernel appends it.
Both print one `trace:` line per call from the same kernel code,
and a `user fault` line for each fault of a driver thread,
which stops that thread alone: when the records unmapped it,
and when it finds the records done, since the driver ends with a breakpoint.
The fault's frame is left out, since the host knows no address of the driver's code.
A terminal line follows:
`no runnable thread` when every thread of the driver has faulted or blocked,
`user halt with code` when a record halted the machine,
`untraced thread` when the records started a thread
whose code the host cannot follow,
`invariant violated`,
or `kernel panic`.
The traces must match exactly,
and a trace ending in `invariant violated` or `kernel panic` fails its input
even when both builds agree on it.

The host replays its share of the inputs in one process,
each as if it ran alone, see host/fuzz.c;
its reports on stderr are part of its trace,
since host/history.c reports there what the self-check cannot see.
An input that crashes the host ends that process and fails,
and the rest go on in the next.
"""

import argparse
import concurrent.futures
import os
import shlex
import struct
import subprocess
import sys
import tempfile

# Must match kernel/board/qemu/board.h and include/rvuos/abi.h.
INPUT_BASE = 0x80120000
REPLAY_MAGIC = 0x5A465652
RECORD_SIZE = 16
MAX_RECORDS = 256
TIMEOUT = 20
# The line the host prints after each input it replays isolated; HOST_INPUT_END in host/harness.h.
INPUT_END = "isolated: end of input\n"

# Lines kept wherever they come, and lines that end the trace.
EVENT_PREFIXES = ("trace:", "user fault")
TERMINAL_PREFIXES = (
    "user halt with code",
    "no runnable thread",
    "untraced thread",
    "invariant violated",
    "kernel panic",
)
# What the host's trace ends in when the input crashed it, which no QEMU trace holds.
HOST_DIED = "host harness died"


def decode(raw: bytes) -> str:
    """The log may contain arbitrary bytes from OP_DEBUG_WRITE."""
    return raw.decode("utf-8", errors="replace")


def relevant_lines(output: str) -> list[str]:
    """Keep trace and fault lines and the first terminal line, drop everything else."""
    lines = []
    for line in output.splitlines():
        line = line.strip()
        if line.startswith(EVENT_PREFIXES):
            lines.append(line)
        elif line.startswith(TERMINAL_PREFIXES):
            lines.append(line)
            break
    return lines


def make_blob(data: bytes) -> bytes:
    count = min(len(data) // RECORD_SIZE, MAX_RECORDS)
    return struct.pack("<II", REPLAY_MAGIC, count) + data[: count * RECORD_SIZE]


def run_host(host_bin: str, paths: list[str]) -> list[list[str]]:
    """The host's trace of each input, from as few processes as the inputs let it."""
    traces = []
    while len(traces) < len(paths):
        rest = paths[len(traces):]
        proc = subprocess.run(
            [host_bin, "--verbose", "--isolated", *rest],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=TIMEOUT * len(rest), check=False,
        )
        parts = decode(proc.stdout).split(INPUT_END)
        ended = min(len(parts) - 1, len(rest))
        traces += [relevant_lines(part) for part in parts[:ended]]
        # What follows the last end is the input the process died in, if one did.
        if ended < len(rest):
            traces.append(relevant_lines(parts[ended]) + [HOST_DIED])
    return traces


def run_qemu(qemu: list[str], kernel: str, data: bytes) -> list[str]:
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(make_blob(data))
        blob = f.name
    try:
        cmd = qemu + [
            "-bios", kernel,
            "-device", f"loader,file={blob},addr={INPUT_BASE:#x}",
        ]
        proc = subprocess.run(
            cmd, capture_output=True, timeout=TIMEOUT,
            stdin=subprocess.DEVNULL, check=False,
        )
        return relevant_lines(decode(proc.stdout))
    finally:
        os.unlink(blob)


def compare(name: str, qemu: list[str], kernel: str, host: list[str], path: str):
    with open(path, "rb") as f:
        data = f.read()
    target = run_qemu(qemu, kernel, data)
    if host == target:
        if host and host[-1].startswith("invariant violated"):
            return name, "INVARIANT", "  both builds report: " + host[-1]
        if host and host[-1].startswith("kernel panic"):
            return name, "PANIC", "  both builds report: " + host[-1]
        return name, None, None
    diff = []
    for i in range(max(len(host), len(target))):
        h = host[i] if i < len(host) else "<none>"
        t = target[i] if i < len(target) else "<none>"
        if h != t:
            diff.append(f"  line {i}:\n    host: {h}\n    qemu: {t}")
            if len(diff) >= 3:
                break
    return name, "MISMATCH", "\n".join(diff)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", required=True, help="QEMU command line without -bios")
    ap.add_argument("--kernel", required=True, help="kernel image with the replay driver")
    ap.add_argument("--host", required=True, help="host fuzz binary")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--fail-fast", action="store_true",
                    help="stop at the first input that fails")
    ap.add_argument("inputs", nargs="+", help="input files or directories")
    args = ap.parse_args()

    paths = []
    for p in args.inputs:
        if os.path.isdir(p):
            paths += sorted(
                os.path.join(p, n) for n in os.listdir(p) if not n.startswith(".")
            )
        else:
            paths.append(p)

    qemu = shlex.split(args.qemu)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        share = max(1, -(-len(paths) // args.jobs))
        shares = [paths[i:i + share] for i in range(0, len(paths), share)]
        host = [t for ts in pool.map(lambda s: run_host(args.host, s), shares) for t in ts]
        futures = [
            pool.submit(compare, os.path.basename(p), qemu, args.kernel, h, p)
            for p, h in zip(paths, host)
        ]
        for fut in concurrent.futures.as_completed(futures):
            name, kind, detail = fut.result()
            if kind is not None:
                failures += 1
                print(f"{kind} {name}\n{detail}")
                if args.fail_fast:
                    # The inputs not yet begun are dropped; those under way finish unseen.
                    for f in futures:
                        f.cancel()
                    print("stopped at the first input that fails")
                    return 1

    print(f"{len(paths) - failures}/{len(paths)} inputs pass:"
          " host and QEMU agree and no invariant is violated")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
