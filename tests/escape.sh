#!/bin/sh
# Boot one escape-attempt scenario and check that the hardware stops it.
# Usage: tests/escape.sh "<command that boots it>" <scenario name>
# The command is QEMU on BOARD=qemu and tools/esp32c6-run.py on BOARD=esp32c6;
# either exits with the status the kernel halted with.
#
# Each scenario is a root task that attempts one escape and, were it not stopped,
# says so and halts with status 2; see user/escape-*.c.
# A fault stops the thread that makes it and the kernel reports it in its log;
# the root task's thread is the only one, so the kernel then halts with status 5,
# no runnable thread, and writes the log out, fault report and all; see DESIGN.md, "Faults".
# PMP is what confines a process, so this runs on the target, like `make test`.

set -eu

boot_cmd=$1
scenario=$2
log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
timeout 10 sh -c "$boot_cmd" > "$log" 2>&1
status=$?
set -e

cat "$log"

fail() {
    echo "FAIL ($scenario): $1" >&2
    exit 1
}

# Each scenario says where its escape is as it reaches it, "... at 0x..., expecting a fault",
# and one that reaches memory names that address just before, "... 0x... at 0x...":
# the fault must name the first in mepc, and the second in mtval where the case asks.
# A scenario prints "escape: breached ..." only if the escape went through.
addr=$(sed -n 's/.*escape: .* at \(0x[0-9a-f]*\), expecting a fault.*/\1/p' "$log" | head -1)
target=$(sed -n 's/.*escape: .* \(0x[0-9a-f]*\) at 0x[0-9a-f]*, expecting a fault.*/\1/p' "$log" | head -1)
case "$scenario" in
escape-execute-data | escape-jump-kernel) fault="mcause=0x00000001 mepc=$addr mtval=$addr" ;;
escape-csrr | escape-mret) fault="mcause=0x00000002 mepc=$addr" ;; # whatever mtval holds
escape-misaligned-load) fault="mcause=0x00000005 mepc=$addr" ;; # mtval is the board's
escape-misaligned-store) fault="mcause=0x00000007 mepc=$addr" ;; # mtval is the board's
escape-store-kernel) fault="mcause=0x00000007 mepc=$addr mtval=$target" ;;
*) fail "unknown scenario" ;;
esac

grep -q 'rvuos: machine mode up' "$log" || fail "kernel did not boot"
grep -q 'rvuos: halting, the log follows' "$log" || fail "the halt did not write the log out"
grep -q ': FAILED' "$log" && fail "a setup step failed before the escape"
[ -n "$addr" ] || fail "the scenario did not reach the escape"
grep -q 'escape: breached' "$log" && fail "the hardware did not stop the escape"
grep -q 'user fault' "$log" || fail "the escape did not fault"
grep -Eq "$fault([[:space:]]|\$)" "$log" || fail "the fault was not the expected $fault"
grep -q 'no runnable thread' "$log" || fail "the machine did not stop for want of a thread after the fault"
[ "$status" -eq 5 ] || fail "expected exit status 5 (no runnable thread after the fault), got $status"

echo "PASS ($scenario)"
