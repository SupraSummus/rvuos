#!/bin/sh
# Boot the kernel and check the expected transcript.
# Usage: tests/run.sh "<command that boots it>" <PMP entries the kernel may use>
# The command is QEMU on BOARD=qemu and tools/esp32c6-run.py on BOARD=esp32c6;
# either exits with the status the kernel halted with.

set -eu

boot_cmd=$1
max_entries=$2
log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
# The demo takes a few seconds; the rest is for a loaded machine, as under `make mutants`.
timeout 30 sh -c "$boot_cmd" > "$log" 2>&1
status=$?
set -e

cat "$log"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# The demo ends in a halt with code 0, after a fault that stopped one thread and not the machine.
[ "$status" -eq 0 ] || fail "expected exit status 0 (the demo's own halt), got $status"
# The kernel has no console: its log reaches the UART through the root task's logger
# while the machine runs, and the halt writes the whole log out after it, under this line.
# What the logger carried out therefore comes before the line, what only the halt did after.
dump=$(grep -n 'rvuos: halting, the log follows' "$log" | head -1 | cut -d: -f1)
[ -n "$dump" ] || fail "the halt did not write the log out"
# The first occurrence of a line, for comparing against the dump's position.
first() { grep -n "$1" "$log" | head -1 | cut -d: -f1; }
grep -q 'rvuos: machine mode up' "$log" || fail "kernel did not boot"
# QEMU and the ESP32-C6 implement sixteen entries with a four-byte grain,
# of which the probe must find as many as the kernel may use.
entries=$(printf '0x%08x' $((max_entries < 16 ? max_entries : 16)))
grep -q "rvuos: pmp entries $entries grain 0x00000004" "$log" \
    || fail "PMP probe did not report $entries entries and a four-byte grain"
grep -q 'the layout fits the smallest region: ok' "$log" \
    || fail "the root task did not see the smallest region"
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
    || fail "revoking below a frame did not take what was derived and installed from it"
grep -q 'child: lease revoked here too' "$log" \
    || fail "the revoke did not reach the derived capability in the other process's table"
grep -q 'child: pool made' "$log" || fail "the child could not pool the lent memory"
grep -q 'root: cascade ok' "$log" \
    || fail "revoking the lent memory did not destroy the pool the child made of it"
grep -q 'root: timer ok' "$log" \
    || fail "the timer did not wake the only thread from its sleep"
grep -q 'root: clock ok' "$log" \
    || fail "the clock's counter, read through its region, did not show the sleeps' length"
grep -q 'root: period ok' "$log" \
    || fail "a periodic timer line drifted from its period"
grep -q 'root: units ok' "$log" \
    || fail "threads on spare time took the time a thread earned, or it ran past its units"
grep -q 'root: unbind ok' "$log" \
    || fail "a thread ran after its units were revoked"
grep -q 'root: rebind ok' "$log" \
    || fail "a thread bound to units again did not run, or another ran with it"
grep -q 'root: spare ok' "$log" \
    || fail "a thread without spare time ran past its units, or one with it did not run on spare time"
grep -q 'root: charge ok' "$log" \
    || fail "a thread that sleeps across every tick ran past its units"
grep -q 'root: tickless ok' "$log" \
    || fail "the timer interrupted the only thread to run at ticks that changed nothing"
grep -q 'the idle line stays quiet: ok' "$log" \
    || fail "an armed line nothing raises signalled"
grep -q 'root: irq ok' "$log" \
    || fail "destroying the irq's pool did not free its line"
grep -q 'the root task cannot destroy its own pool: ok' "$log" \
    || fail "a thread destroyed the pool it lives in"
grep -q 'root: handover ok' "$log" \
    || fail "a successor given everything the root task held could not destroy the root task and take its place"
grep -q 'root: fault ok' "$log" \
    || fail "a fault stopped more than its thread, its watch did not hear it, or a resume did not run the load again"
# The logger carried the kernel's banner and the root task's output to the UART itself,
# one byte per interrupt, before the halt wrote the log out.
# The fault comes right after the last lines, so those the halt may be first to carry;
# the root task sleeps after the timer line, which is when the logger catches up.
[ "$(first 'rvuos: machine mode up')" -lt "$dump" ] \
    || fail "the logger did not carry the kernel's log out before the halt did"
[ "$(first 'root: timer ok')" -lt "$dump" ] \
    || fail "the logger did not carry the root task's output out before the halt did"
grep -q 'user fault' "$log" || fail "PMP fault was not caught"
# The successor says where its prober reads; the fault must name that address.
addr=$(sed -n 's/.*reading the removed region at \(0x[0-9a-f]*\),.*/\1/p' "$log" | head -1)
[ -n "$addr" ] || fail "the successor did not say where its prober reads"
grep -q "mcause=0x00000005 mepc=0x........ mtval=$addr" "$log" \
    || fail "fault was not a load access fault on the removed region from user code"
grep -q ': FAILED' "$log" && fail "a step failed"

echo "PASS"
