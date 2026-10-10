#!/bin/sh
# Boot one escape-attempt scenario and check that the hardware stops it.
# Usage: tests/escape.sh "<command that boots it>" <scenario name>
# The command is QEMU on BOARD=qemu and the board's runner in tools/ on the others;
# each exits with the status the kernel halted with.
#
# Each scenario is a root task that attempts one escape and, were it not stopped,
# says so and halts with status 2; see user/escape-*.c.
# A fault stops the thread that makes it and the kernel reports it in its log;
# the root task's thread is the only one, so the kernel then halts with status 5,
# no runnable thread, and writes the log out, fault report and all; see DESIGN.md, "Faults".
# PMP or the MPU is what confines a process, so this runs on the target, like `make test`.
# The scenarios say what the specification has a core report;
# BOARD_MTVAL and BOARD_MISALIGNED say where the board's differs, see tests/run.sh,
# and BOARD_ARCH=arm that the fault is reported as ARM has it, see kernel/arch/arm/frame.c,
# BOARD_STACKING=call that the core enters a call whose frame it cannot stack as the call, not the MemManage.

set -eu

boot_cmd=$1
scenario=$2
board_mtval=${BOARD_MTVAL:-address}
board_misaligned=${BOARD_MISALIGNED:-split}
board_stacking=${BOARD_STACKING:-fault}
board_arch=${BOARD_ARCH:-riscv}
log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
timeout 10 sh -c "$boot_cmd" > "$log" 2>&1
status=$?
set -e

fail() {
    cat "$log"
    echo "FAIL ($scenario): $1" >&2
    exit 1
}

# Each scenario says where its escape is as it reaches it, "... at 0x..., expecting a fault",
# and one that reaches memory names that address just before, "... 0x... at 0x...":
# the fault must name the first in mepc, and the second in mtval where the case asks,
# or on ARM in pc and in the address MMFAR or BFAR holds.
# A scenario prints "escape: breached ..." only if the escape went through.
addr=$(sed -n 's/.*escape: .* at \(0x[0-9a-f]*\), expecting a fault.*/\1/p' "$log" | head -1)
target=$(sed -n 's/.*escape: .* \(0x[0-9a-f]*\) at 0x[0-9a-f]*, expecting a fault.*/\1/p' "$log" | head -1)
# What mtval shows for an address, and the causes of a misaligned load and store:
# access faults where the core splits the access, misaligned exceptions where it traps.
if [ "$board_mtval" = address ]; then addr_mtval=$addr target_mtval=$target
else addr_mtval=0x00000000 target_mtval=0x00000000; fi
if [ "$board_misaligned" = split ]; then load_cause=0x00000005 store_cause=0x00000007
else load_cause=0x00000004 store_cause=0x00000006; fi
# ARM's are the exception and the configurable fault status:
# a MemManage, 4, for what the MPU refuses, IACCVIOL, 0x1, on a fetch,
# DACCVIOL with MMFAR valid, 0x82, on data, and MSTKERR, 0x10, or MUNSTKERR, 0x8, on the core's own stacking,
# which name no address, and a BusFault, 5, precise with BFAR valid, 0x8200, on the System Control Space.
if [ "$board_stacking" = call ]; then stack_call=0x0000000b; else stack_call=0x00000004; fi
if [ "$board_arch" = arm ]; then
case "$scenario" in
escape-execute-data | escape-jump-kernel) fault="exception=0x00000004 cfsr=0x00000001 pc=$addr addr=0x00000000" ;;
escape-misaligned-load | escape-misaligned-store) fault="exception=0x00000004 cfsr=0x00000082 pc=$addr" ;;
escape-store-kernel | escape-store-clock | escape-past-region)
    fault="exception=0x00000004 cfsr=0x00000082 pc=$addr addr=$target" ;;
escape-load-scs) fault="exception=0x00000005 cfsr=0x00008200 pc=$addr addr=$target" ;;
# The core wrote no frame, so the pc is what the thread's last trap left, which the scenario names;
# nRF52840's Cortex-M4 enters the SVCall, 11, and QEMU's cores and the Cortex-M33 the MemManage, either a fault to the kernel.
escape-stack-call) fault="exception=$stack_call cfsr=0x00000010 pc=$addr addr=0x00000000" ;;
escape-unstack) fault="exception=0x00000004 cfsr=0x00000008 pc=$addr addr=0x00000000" ;;
*) fail "unknown scenario" ;;
esac
else
case "$scenario" in
escape-execute-data | escape-jump-kernel) fault="mcause=0x00000001 mepc=$addr mtval=$addr_mtval" ;;
escape-csrr | escape-mret) fault="mcause=0x00000002 mepc=$addr" ;; # whatever mtval holds
escape-misaligned-load) fault="mcause=$load_cause mepc=$addr" ;; # mtval is the board's
escape-misaligned-store) fault="mcause=$store_cause mepc=$addr" ;; # mtval is the board's
escape-store-kernel | escape-store-clock) fault="mcause=0x00000007 mepc=$addr mtval=$target_mtval" ;;
escape-past-region) fault="mcause=0x00000005 mepc=$addr mtval=$target_mtval" ;;
*) fail "unknown scenario" ;;
esac
fi

grep -q 'rvuos: .* mode up' "$log" || fail "kernel did not boot"
grep -q 'rvuos: halting, the log follows' "$log" || fail "the halt did not write the log out"
grep -q ': FAILED' "$log" && fail "a setup step failed before the escape"
[ -n "$addr" ] || fail "the scenario did not reach the escape"
grep -q 'escape: breached' "$log" && fail "the hardware did not stop the escape"
grep -q 'user fault' "$log" || fail "the escape did not fault"
grep -Eq "$fault([[:space:]]|\$)" "$log" || fail "the fault was not the expected $fault"
grep -q 'no runnable thread' "$log" || fail "the machine did not stop for want of a thread after the fault"
[ "$status" -eq 5 ] || fail "expected exit status 5 (no runnable thread after the fault), got $status"

echo "PASS ($scenario)"
