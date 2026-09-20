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
# The kernel has no console: its log reaches the UART through the root task's logger
# while the machine runs, and the halt writes the whole log out after it, under this line.
# What the logger carried out therefore comes before the line, what only the halt did after.
dump=$(grep -n 'rvuos: halting, the log follows' "$log" | head -1 | cut -d: -f1)
[ -n "$dump" ] || fail "the halt did not write the log out"
# The first occurrence of a line, for comparing against the dump's position.
first() { grep -n "$1" "$log" | head -1 | cut -d: -f1; }
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
grep -q 'root: derivation ok' "$log" \
    || fail "revoking below a region capability did not take what was derived and installed from it"
grep -q 'child: lease revoked here too' "$log" \
    || fail "the revoke did not reach the derived capability in the other process's table"
grep -q 'child: pool made' "$log" || fail "the child could not pool the lent memory"
grep -q 'child: cannot destroy the pool above' "$log" \
    || fail "a thread destroyed a pool its own lies below"
grep -q 'root: cascade ok' "$log" \
    || fail "destroying the child's pool did not take the pool the child made"
grep -q 'root: timer ok' "$log" \
    || fail "the timer did not wake the only thread from its sleep"
grep -q 'the idle line stays quiet: ok' "$log" \
    || fail "an armed line nothing raises signalled"
grep -q 'root: irq ok' "$log" \
    || fail "destroying the irq's pool did not free its line"
# The logger carried the kernel's banner and the root task's output to the UART itself,
# one byte per interrupt, before the halt wrote the log out.
# The fault comes right after the last lines, so those the halt may be first to carry;
# the root task sleeps after the timer line, which is when the logger catches up.
[ "$(first 'rvuos: machine mode up')" -lt "$dump" ] \
    || fail "the logger did not carry the kernel's log out before the halt did"
[ "$(first 'root: timer ok')" -lt "$dump" ] \
    || fail "the logger did not carry the root task's output out before the halt did"
grep -q 'user fault' "$log" || fail "PMP fault was not caught"
# mepc's low bits move with the code layout.
grep -q 'mcause=0x00000005 mepc=0x8010.... mtval=0x80210000' "$log" \
    || fail "fault was not a load access fault on the removed region from user code"
grep -q ': FAILED' "$log" && fail "a step failed"
grep -q 'PMP did not stop the read' "$log" && fail "user read kernel memory"

echo "PASS"
