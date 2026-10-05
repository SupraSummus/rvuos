#!/bin/sh
# Boot the laboratory, user/lab/, and print what its scenarios measured, which it holds to nothing.
# Usage: tests/lab.sh "<command that boots it>"
# The command is QEMU on the QEMU boards and the board's runner in tools/ on the others;
# each exits with the status the kernel halted with.
# The run ends with "lab: done" and halt code 0, or with what failed and another code.

set -eu

boot_cmd=$1
log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
timeout 120 sh -c "$boot_cmd" > "$log" 2>&1
status=$?
set -e

fail() {
    cat "$log"
    echo "FAIL (lab): $1" >&2
    exit 1
}

[ "$status" -eq 0 ] || fail "expected exit status 0 (the run's own halt), got $status"
grep -q 'rvuos: the console lost' "$log" && fail "the console overflowed, and the lines past its end are lost"
grep -q 'lab: done' "$log" || fail "the run did not finish"
# What the scenarios measured, as a table, in microseconds.
grep '^lab: .* n=' "$log" | sed 's/^lab: //; s/[a-z0-9]*=//g' |
    awk 'BEGIN { print "scenario     member      n  median     p90   worst" }
         { printf "%-12s %-6s %5s %7s %7s %7s\n", $1, $2, $3, $4, $5, $6 }'
