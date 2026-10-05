#!/usr/bin/env python3
"""Boot user/bench/ and say what a round of each of its measures costs, in the core's cycles.

The program counts by the clock, whose rate differs from board to board, where cycles do not:
--cpu-hz is the core's clock, `clock` where the clock counts the core's cycles itself, as on the ESP32-C6,
or `icount` under QEMU, whose -icount shift=0 has the clock count instructions, which are the costs there.
Each cost is held to the record within a tenth either way, a band one clang's code against another's stays in;
a build with no record, a chip's, prints its costs alone, and --refresh writes the record from the run.
"""

import argparse
import os
import re
import subprocess
import sys

BAND = 0.10
LINE = re.compile(r"bench: (\S+) rounds=(\d+) counts=(\d+) hz=(\d+)")


def costs(output, cpu_hz):
    found = {}
    for name, rounds, counts, hz in LINE.findall(output):
        if cpu_hz == "icount":
            per_count = 1e9 / int(hz)
        elif cpu_hz == "clock":
            per_count = 1
        else:
            per_count = int(cpu_hz) / int(hz)
        found[name] = round(int(counts) * per_count / int(rounds))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--cpu-hz", required=True, help="the core's cycles in a second, clock, or icount")
    parser.add_argument("--board", required=True)
    parser.add_argument("--record", required=True, help="the costs to hold the run to, if the file exists")
    parser.add_argument("--refresh", action="store_true", help="write the record from the run")
    parser.add_argument("boot", help="the command that boots the image and exits with its halt code")
    args = parser.parse_args()
    unit = "instructions" if args.cpu_hz == "icount" else "cycles"

    try:
        run = subprocess.run(args.boot, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    except subprocess.TimeoutExpired:
        sys.exit("FAIL (bench): no halt within 120 s")
    output = run.stdout.decode(errors="replace")
    if run.returncode != 0 or "bench: done" not in output:
        sys.stdout.write(output)
        sys.exit(f"FAIL (bench): the run did not finish, exit status {run.returncode}")
    found = costs(output, args.cpu_hz)

    if args.refresh:
        with open(args.record, "w") as f:
            f.write(f"# A round of each measure of user/bench/ on {args.board}, in {unit}; see tests/bench.py.\n")
            f.writelines(f"{name} {value}\n" for name, value in found.items())
    record = {}
    if os.path.exists(args.record):
        with open(args.record) as f:
            record = dict(line.split() for line in f if not line.startswith("#"))

    print(f"bench: {args.board}, in {unit}, a round each" + ("" if record else "; no record to hold them to"))
    failed = [name for name in record if name not in found]
    for name, value in found.items():
        line = f"  {name:16} {value:>10}"
        if name in record:
            was = int(record[name])
            line += f"   record {was:>10}  {(value - was) / was:+.1%}"
            if abs(value - was) > BAND * was:
                failed.append(name)
                line += "  <- moved more than a tenth"
        print(line)
    if failed:
        sys.exit(
            f"FAIL (bench): {', '.join(failed)}, against {args.record};\n"
            "if the change means it, make bench-refresh writes the record anew, and its commit says what it cost"
        )


if __name__ == "__main__":
    main()
