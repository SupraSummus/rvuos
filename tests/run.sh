#!/bin/sh
# Boot the kernel under QEMU and check the expected transcript.
# Usage: tests/run.sh "<qemu command line>"

set -eu

qemu_cmd=$1
log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
timeout 10 sh -c "$qemu_cmd" > "$log" 2>&1
status=$?
set -e

cat "$log"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

[ "$status" -eq 4 ] || fail "expected exit status 4 (user fault), got $status"
grep -q 'rvuos: machine mode up' "$log" || fail "kernel did not boot"
grep -q 'hello from user mode' "$log" || fail "user mode did not run"
grep -q 'user fault' "$log" || fail "PMP fault was not caught"
# mepc's low bits move with the code layout.
grep -q 'mcause=0x00000005 mepc=0x8010.... mtval=0x80211000' "$log" \
    || fail "fault was not a load access fault on the removed region from user code"
grep -q ': FAILED' "$log" && fail "a step failed"
grep -q 'PMP did not stop the read' "$log" && fail "user read kernel memory"

echo "PASS"
