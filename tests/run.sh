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
# QEMU implements sixteen entries with a four-byte grain; the probe must find exactly that.
grep -q 'rvuos: pmp entries 0x00000010 grain 0x00000004' "$log" \
    || fail "PMP probe did not report sixteen entries and a four-byte grain"
grep -q 'the layout fits the grain: ok' "$log" || fail "the root task did not see the grain"
grep -q 'hello from user mode' "$log" || fail "user mode did not run"
grep -q 'root: message ok' "$log" || fail "the child's message did not arrive"
grep -q 'child: reply ok' "$log" || fail "the root task's answer did not arrive"
grep -q 'root: preemption ok' "$log" \
    || fail "the tick did not take the processor from a spinning thread"
grep -q 'root: revocation ok' "$log" \
    || fail "a destroyed pool did not revoke the capabilities into it"
grep -q 'child: revoked here too' "$log" \
    || fail "revocation did not reach the other process's table"
grep -q 'child: pool made' "$log" || fail "the child could not pool the lent memory"
grep -q 'child: cannot destroy the pool above' "$log" \
    || fail "a thread destroyed a pool its own lies below"
grep -q 'root: cascade ok' "$log" \
    || fail "destroying the child's pool did not take the pool the child made"
grep -q 'user fault' "$log" || fail "PMP fault was not caught"
# mepc's low bits move with the code layout.
grep -q 'mcause=0x00000005 mepc=0x8010.... mtval=0x80212000' "$log" \
    || fail "fault was not a load access fault on the removed region from user code"
grep -q ': FAILED' "$log" && fail "a step failed"
grep -q 'PMP did not stop the read' "$log" && fail "user read kernel memory"

echo "PASS"
