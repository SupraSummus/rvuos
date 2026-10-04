#!/bin/sh
# Boot the kernel and check the expected transcript.
# Usage: tests/run.sh "<command that boots it>" <PMP entries the kernel may use>
# The command is QEMU on BOARD=qemu and the board's runner in tools/ on the others;
# each exits with the status the kernel halted with.
# A board whose core differs sets BOARD_FACTS in the Makefile: the PMP entries and grain the probe finds,
# BOARD_MTVAL=zero where mtval reads zero, BOARD_MISALIGNED=trap for tests/escape.sh,
# and BOARD_ARCH=arm where the fault is reported as ARMv7-M has it.
# BOARD_CORES is how many cores the kernel runs on, whose demo goes on to the second.

set -eu

boot_cmd=$1
max_entries=$2
board_entries=${BOARD_PMP_ENTRIES:-16}
board_grain=${BOARD_PMP_GRAIN:-4}
board_mtval=${BOARD_MTVAL:-address}
board_arch=${BOARD_ARCH:-riscv}
board_cores=${BOARD_CORES:-1}
log=$(mktemp)
text=$(mktemp)
trap 'rm -f "$log" "$text"' EXIT

set +e
# The demo takes a few seconds, twice that on two harts; the rest is for a loaded machine, as under `make mutants`.
timeout 60 sh -c "$boot_cmd" > "$log" 2>&1
status=$?
set -e

cat "$log"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# The demo ends when the watchdog it stopped feeding halts the machine, with code 7,
# after a fault that stopped one thread and not the machine.
[ "$status" -eq 7 ] || fail "expected exit status 7 (the watchdog's halt), got $status"
# RP2350's console is RAM of a fixed size, and its halt says how much did not fit, which would pass for a missing line.
grep -q 'rvuos: the console lost' "$log" && fail "the console overflowed, and the lines past its end are lost"
# The kernel has no console: its log reaches the UART through the root task's logger
# while the machine runs, and the halt writes the whole log out after it, under this line.
# What the logger carried out therefore comes before the line, what only the halt did after.
dump=$(grep -n 'rvuos: halting, the log follows' "$log" | head -1 | cut -d: -f1)
[ -n "$dump" ] || fail "the halt did not write the log out"
# The first occurrence of a line, for comparing against the dump's position.
first() { grep -n "$1" "$log" | head -1 | cut -d: -f1; }
# The logger stops wherever its last turn ended, which may be within a line,
# and the halt's line then lies between the line's two halves;
# what was written is read with that line taken out and the halves joined again.
awk -v at="$dump" '
NR == at - 1 { sub(/\r$/, ""); partial = $0; next }
NR == at { next }
NR == at + 1 { print partial $0; next }
{ print }
' "$log" > "$text"
grep -q 'rvuos: .* mode up' "$text" || fail "kernel did not boot"
# The probe must find as many of the core's entries as the kernel may use.
entries=$(printf '0x%08x' $((max_entries < board_entries ? max_entries : board_entries)))
grain=$(printf '0x%08x' "$board_grain")
if [ "$board_arch" = arm ]; then regions="mpu regions"; else regions="pmp entries"; fi
grep -q "rvuos: $regions $entries grain $grain" "$text" \
    || fail "the protection unit's probe did not report $entries entries and a grain of $grain"
grep -q 'the layout fits the smallest region: ok' "$text" \
    || fail "the root task did not see the smallest region"
grep -q 'hello from user mode' "$text" || fail "user mode did not run"
grep -q 'root: message ok' "$text" || fail "the child's message did not arrive"
grep -q 'child: reply ok' "$text" || fail "the root task's answer did not arrive"
grep -q 'child: may not halt ok' "$text" || fail "a child given the log to write could halt the machine"
grep -q 'root: preemption ok' "$text" \
    || fail "the tick did not take the processor from a spinning thread"
grep -q "the counter counts on across the child's turns: ok" "$text" \
    || fail "the performance counter the root task started did not count on across another process's turns"
grep -q 'root: revocation ok' "$text" \
    || fail "a destroyed pool did not revoke the capabilities into it"
grep -q 'child: revoked here too' "$text" \
    || fail "revocation did not reach the other process's table"
grep -q 'root: derivation ok' "$text" \
    || fail "revoking below a frame did not take what was derived and installed from it"
grep -q 'child: lease revoked here too' "$text" \
    || fail "the revoke did not reach the derived capability in the other process's table"
grep -q 'child: pool made' "$text" || fail "the child could not pool the lent memory"
grep -q 'root: cascade ok' "$text" \
    || fail "revoking the lent memory did not destroy the pool the child made of it"
grep -q 'root: timer ok' "$text" \
    || fail "the timer did not wake the only thread from its sleep"
grep -q 'root: clock ok' "$text" \
    || fail "the clock's counter, read through its region, did not show the sleeps' length"
grep -q 'root: long sleep ok' "$text" \
    || fail "a sleep longer than one reload of the compare never woke the idle kernel"
grep -q 'root: period ok' "$text" \
    || fail "a periodic timer line drifted from its period"
grep -q 'root: units ok' "$text" \
    || fail "threads on spare time took the time a thread earned, or it ran past its units"
grep -q 'root: unbind ok' "$text" \
    || fail "a thread ran after its units were revoked"
grep -q 'root: rebind ok' "$text" \
    || fail "a thread bound to units again did not run, or another ran with it"
grep -q 'root: spare ok' "$text" \
    || fail "a thread without spare time ran past its units, or one with it did not run on spare time"
grep -q 'root: charge ok' "$text" \
    || fail "a thread that sleeps across every tick ran past its units"
grep -q 'root: tickless ok' "$text" \
    || fail "the timer interrupted the only thread to run at ticks that changed nothing"
grep -q 'the idle line stays quiet: ok' "$text" \
    || fail "an armed line nothing raises signalled"
grep -q 'root: irq ok' "$text" \
    || fail "destroying the irq's pool did not free its line"
grep -q "root: cores $(printf '0x%08x' "$board_cores")" "$text" \
    || fail "the root task did not find the $board_cores cores in the units it was granted"
if [ "$board_cores" -gt 1 ]; then
    grep -q 'root: second core ok' "$text" \
        || fail "a thread bound to the second core's units did not run beside the root task"
    grep -q 'root: move ok' "$text" \
        || fail "a thread moved from the second core to an eighth of the first ran more than that there, or not at all"
    grep -q 'root: wake across cores ok' "$text" \
        || fail "a signal did not wake a thread that waited on the idle second core"
    grep -q 'root: shootdown ok' "$text" \
        || fail "a thread on the second core went on reading a region taken from its process"
    grep -q 'root: taken from its core ok' "$text" \
        || fail "a thread destroyed while it ran on the second core ran on, or the core ran nothing after it"
fi
grep -q 'the root task cannot destroy its own pool: ok' "$text" \
    || fail "a thread destroyed the pool it lives in"
grep -q 'successor: the user-mode csrs set back: ok' "$text" \
    || fail "the user-mode CSRs the root task marked reached the successor's process"
grep -q 'root: handover ok' "$text" \
    || fail "a successor given everything the root task held could not destroy the root task and take its place"
grep -q 'root: fault ok' "$text" \
    || fail "a fault stopped more than its thread, its watch did not hear it, a resume did not run the load again, or its registers did not move it on"
grep -q 'root: watchdog fed ok' "$text" \
    || fail "the watchdog halted the machine although it was fed in time"
grep -q 'watchdog: not fed in time' "$text" \
    || fail "the watchdog did not halt the machine once it was not fed"
# The logger carried the kernel's banner and the root task's output to the UART itself,
# one byte per interrupt, before the halt wrote the log out.
# The fault comes right after the last lines, so those the halt may be first to carry;
# the root task sleeps after the timer line, which is when the logger catches up.
[ "$(first 'rvuos: .* mode up')" -lt "$dump" ] \
    || fail "the logger did not carry the kernel's log out before the halt did"
[ "$(first 'root: timer ok')" -lt "$dump" ] \
    || fail "the logger did not carry the root task's output out before the halt did"
grep -q 'user fault' "$text" || fail "PMP fault was not caught"
# The successor says where its prober reads; the fault must name that address.
addr=$(sed -n 's/.*reading the removed region at \(0x[0-9a-f]*\),.*/\1/p' "$text" | head -1)
[ -n "$addr" ] || fail "the successor did not say where its prober reads"
[ "$board_mtval" = address ] || addr=0x00000000
# The prober's fault is the first after the successor says so;
# on two cores a reader faulted before it, which where mtval reads zero looks the same.
prober=$(sed -n '/reading the removed region at/,$p' "$text")
# RISC-V says a load access fault; ARMv7-M a MemManage, a data access violation with its address valid.
if [ "$board_arch" = arm ]; then load_fault="exception=0x00000004 cfsr=0x00000082 pc=0x........ addr=$addr"
else load_fault="mcause=0x00000005 mepc=0x........ mtval=$addr"; fi
printf '%s\n' "$prober" | grep -q "$load_fault" \
    || fail "fault was not a load access fault on the removed region from user code"
# OP_THREAD_FAULT told the successor what the kernel reported, the pc with its Thumb bit on ARM.
at=$(printf '%s\n' "$prober" | grep -o "$load_fault" | head -1 | sed 's/.*pc=\(0x[0-9a-f]\{8\}\).*/\1/')
if [ "$board_arch" = arm ]; then told="cause=0x00000004 pc=$(printf '0x%08x' $((at | 1))) addr=$addr status=0x00000082"
else told="cause=0x00000005 pc=$at addr=$addr status=0x00000000"; fi
grep -q "the prober's fault: $told" "$text" \
    || fail "OP_THREAD_FAULT did not tell the load access fault the kernel reported"
grep -q ': FAILED' "$text" && fail "a step failed"

echo "PASS"
