#!/usr/bin/env python3
"""Replay fuzz inputs on QEMU and on the host build and compare the traces.

Each input is a sequence of 16-byte records:
a system call with the thread that makes it, or the tick as OP_DEBUG_TICK.
The host build performs them directly on the kernel's logic,
passing a record to the thread it names as the driver does;
QEMU boots the real kernel with the replay driver as root task
and the input placed in RAM by QEMU's generic loader.
Both print one `trace:` line per call from the same kernel code,
followed by a terminal line: `user fault` when the driver finishes
with its breakpoint or the records unmapped it,
`user halt with code` when a record halted the machine,
`no runnable thread` when every thread of the driver blocked,
`untraced thread` when the records started a thread
whose code the host cannot follow,
or `invariant violated`.
The traces must match exactly,
and a trace ending in `invariant violated` fails its input
even when both builds agree on it.
"""

import argparse
import concurrent.futures
import os
import shlex
import struct
import subprocess
import sys
import tempfile

# Must match kernel/kernel.h and include/rvuos/abi.h.
INPUT_BASE = 0x807F0000
REPLAY_MAGIC = 0x5A465652
RECORD_SIZE = 16
MAX_RECORDS = 256
TIMEOUT = 20

TERMINAL_PREFIXES = (
    "user fault",
    "user halt with code",
    "no runnable thread",
    "untraced thread",
    "invariant violated",
)


def decode(raw: bytes) -> str:
    """Console output may contain arbitrary bytes from OP_DEBUG_PUTC."""
    return raw.decode("utf-8", errors="replace")


def relevant_lines(output: str) -> list[str]:
    """Keep trace lines and the first terminal line, drop everything else."""
    lines = []
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("trace:"):
            lines.append(line)
        elif line.startswith(TERMINAL_PREFIXES):
            lines.append(line)
            break
    return lines


def make_blob(data: bytes) -> bytes:
    count = min(len(data) // RECORD_SIZE, MAX_RECORDS)
    return struct.pack("<II", REPLAY_MAGIC, count) + data[: count * RECORD_SIZE]


def run_host(host_bin: str, path: str) -> list[str]:
    proc = subprocess.run(
        [host_bin, "--verbose", path],
        capture_output=True, timeout=TIMEOUT, check=False,
    )
    return relevant_lines(decode(proc.stdout))


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


def compare(name: str, qemu: list[str], kernel: str, host_bin: str, path: str):
    with open(path, "rb") as f:
        data = f.read()
    host = run_host(host_bin, path)
    target = run_qemu(qemu, kernel, data)
    if host == target:
        if host and host[-1].startswith("invariant violated"):
            return name, "INVARIANT", "  both builds report: " + host[-1]
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
        futures = [
            pool.submit(compare, os.path.basename(p), qemu, args.kernel, args.host, p)
            for p in paths
        ]
        for fut in concurrent.futures.as_completed(futures):
            name, kind, detail = fut.result()
            if kind is not None:
                failures += 1
                print(f"{kind} {name}\n{detail}")

    print(f"{len(paths) - failures}/{len(paths)} inputs pass:"
          " host and QEMU agree and no invariant is violated")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
