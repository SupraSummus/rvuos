#!/bin/sh
# Boot the library's test, user/libtest/, and check what it says of itself.
# Usage: tests/libtest.sh "<command that boots it>"
# The command is QEMU on the QEMU boards and the board's runner in tools/ on the others;
# each exits with the status the kernel halted with.
# The test checks itself and ends with "libtest: ok" and halt code 0,
# or with "libtest: FAIL: ..." or what failed, and another code.

set -eu

boot_cmd=$1
log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
timeout 60 sh -c "$boot_cmd" > "$log" 2>&1
status=$?
set -e

fail() {
    cat "$log"
    echo "FAIL (libtest): $1" >&2
    exit 1
}

[ "$status" -eq 0 ] || fail "expected exit status 0 (the test's own halt), got $status"
grep -q 'rvuos: the console lost' "$log" && fail "the console overflowed, and the lines past its end are lost"
grep -q 'libtest: ok' "$log" || fail "the test did not say it passed"
echo "libtest: passed"
