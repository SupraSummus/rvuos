#!/bin/sh
# Boot the fuel prototype's test, user/fuel/, under a kernel built with make FUEL=1, and check what it says.
# Usage: tests/fuel.sh "<command that boots it>"
# Each stop must come as the fuel runs dry: with no spare time eight times the fuel's length after the start, give or
# take a tenth; with spare time about its length; once refilled between the two, as the account it saved while stopped
# runs first; and a client whose lent time ran dry must have seen KERR_LIMIT, 8.

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
    echo "FAIL (fuel): $1" >&2
    exit 1
}

[ "$status" -eq 0 ] || fail "expected exit status 0 (the run's own halt), got $status"
grep -q 'fuel: done' "$log" || fail "the run did not finish"
grep '^fuel: ' "$log"
check() {
    line=$(grep "^fuel: $1 " "$log") || fail "no $1 line"
    fuel=$(echo "$line" | sed 's/.* fuel=\([0-9]*\) us.*/\1/')
    took=$(echo "$line" | sed 's/.* took=\([0-9]*\) us.*/\1/')
    left=$(echo "$line" | sed 's/.* left=\([0-9]*\).*/\1/')
    [ "$left" -eq 0 ] || fail "$1: fuel left $left after the stop"
    awk -v t="$took" -v f="$fuel" -v lo="$2" -v hi="$3" 'BEGIN { exit !(t >= f * lo * 0.9 && t <= f * hi * 1.1 + 2000) }' ||
        fail "$1: took $took us for $fuel us of fuel, not $2 to $3 times it"
}
check power 8 8
check energy 1 1
check refill 1 8
grep -q '^fuel: lent .* lend=0x00000008 ' "$log" || fail "lent: the client's lend did not end with KERR_LIMIT"
echo "PASS (fuel)"
