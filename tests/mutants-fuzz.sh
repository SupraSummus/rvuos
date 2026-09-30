#!/bin/sh
# Measure how fast the fuzzer finds the planted bugs of tests/mutants/.
#
# `make mutants` requires a seed to catch every mutant,
# which says the checks are strong enough, not that the fuzzer would have found the input.
# Here each mutant the default machine catches is fuzzed from the corpus, the seeds left out,
# or from nothing with -e, for a budget of runs with libFuzzer's seed fixed,
# so that two versions of the mutator or the harness compare by the numbers alone.
# A result is the runs to the first report, "start" when what it starts from reports at once,
# or "-" when the budget ran out; the seconds follow.
# The unmutated kernel is fuzzed first with the same budget and seeds, and must not report.
#
# Usage: make mutants-fuzz MUTANTS_FUZZ='[-j jobs] [-r runs] [-s seeds] [-e] [name...]'
# The table goes to standard output and to build/mutants/fuzz.txt, logs under build/mutants/.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
logs=$root/build/mutants
flags="${FUZZ_FLAGS:?run it as make mutants-fuzz, which passes the flags of make fuzz} -print_final_stats=1"

# One fuzzing run in a tree; prints the runs or "start" or "-", the seconds, and the report.
fuzz_once() {
    dir=$1
    seed=$2
    log=$3
    rm -rf "$dir/start"
    mkdir "$dir/start"
    [ -n "$empty" ] || cp "$root"/tests/corpus/* "$dir/start/"
    start=$(date +%s.%N)
    if (cd "$dir" && build/host/fuzz $flags -runs="$runs" -seed="$seed" start) >"$log" 2>&1; then
        echo "- $(elapsed "$start")"
        return
    fi
    report=$(grep -o 'invariant violated: .*' "$log" | head -1 | sed 's/^invariant violated: //;
        s/ 0x[0-9a-f]\{8\}//g; s/:[0-9][0-9]*//g; s/\<[0-9][0-9]*\>/N/g')
    n=start
    if grep -q 'INITED' "$log"; then
        n=$(grep -o 'stat::number_of_executed_units: [0-9]*' "$log" | grep -o '[0-9]*$')
    fi
    echo "$n $(elapsed "$start") ${report:-no invariant report}"
}

elapsed() {
    echo "$(date +%s.%N) $1" | awk '{ printf "%.1f", $1 - $2 }'
}

# Fuzz one mutant with every seed: a line of results, and the first report after a colon.
mutant() {
    name=$1
    tree=$work/tree-$name
    cp -a "$work/base" "$tree"
    if ! (cd "$tree" && git apply -C1 "$root/tests/mutants/$name.patch" && make -s build/host/fuzz) \
        >"$logs/$name.fuzz.log" 2>&1; then
        echo "$name broken" >"$work/result/$name"
        return
    fi
    line=$name
    first=""
    for seed in $seeds; do
        set -- $(fuzz_once "$tree" "$seed" "$logs/$name.fuzz.$seed.log")
        line="$line $1/$2s"
        shift 2
        [ -n "$first" ] || first=$*
    done
    echo "$line${first:+: $first}" >"$work/result/$name"
}

if [ "${1-}" = --mutant ]; then
    work=$MUTANTS_WORK
    runs=$MUTANTS_RUNS
    seeds=$MUTANTS_SEEDS
    empty=$MUTANTS_EMPTY
    mutant "$2"
    rm -rf "$work/tree-$2"
    exit 0
fi

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
runs=20000
nseeds=1
empty=""
while [ $# -gt 0 ]; do
    case $1 in
        -j) jobs=$2; shift 2 ;;
        -r) runs=$2; shift 2 ;;
        -s) nseeds=$2; shift 2 ;;
        -e) empty=yes; shift ;;
        *) break ;;
    esac
done
seeds=$(seq 1 "$nseeds" | tr '\n' ' ')

[ $# -gt 0 ] || set -- "$root"/tests/mutants/*.patch
names=""
for arg in "$@"; do
    name=$(basename "$arg" .patch)
    patch=$root/tests/mutants/$name.patch
    if [ ! -f "$patch" ]; then
        echo "mutant $name: no such patch" >&2
        exit 1
    fi
    if sed -n '/^diff --git/q; s/^Caught-by: *//p' "$patch" | tr ' ' '\n' | grep -qx fuzz; then
        names="$names $name"
    fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$logs" "$work/base" "$work/result"

cp -r "$root/kernel" "$root/host" "$root/include" "$root/user" "$root/Makefile" "$root/tests" \
    "$root/tools" "$root/DESIGN.md" "$work/base/"
if ! (cd "$work/base" && make -s build/host/fuzz) >"$logs/unmutated.fuzz.log" 2>&1; then
    echo "the unmutated host harness does not build, see build/mutants/unmutated.fuzz.log" >&2
    exit 1
fi
for seed in $seeds; do
    set -- $(fuzz_once "$work/base" "$seed" "$logs/unmutated.fuzz.$seed.log")
    if [ "$1" != - ]; then
        cp "$work/base"/crash-* "$logs/" 2>/dev/null || true
        echo "the unmutated kernel reports with seed $seed after $1 runs, see build/mutants/unmutated.fuzz.$seed.log" >&2
        exit 1
    fi
done
rm -rf "$work/base/start"

export MUTANTS_WORK="$work" MUTANTS_RUNS="$runs" MUTANTS_SEEDS="$seeds" MUTANTS_EMPTY="$empty"
printf '%s\n' $names | xargs -P "$jobs" -I{} "$0" --mutant {}

from="the corpus"
[ -z "$empty" ] || from=nothing
{
    echo "runs to the first report, of $runs, per seed, from $from"
    at_once=0 fuzzed=0 missed=0 broken=0
    for name in $names; do
        line=$(cat "$work/result/$name" 2>/dev/null || echo "$name broken")
        echo "$line"
        case $line in
            *" broken") broken=$((broken + 1)) ;;
            *" start/"*) at_once=$((at_once + 1)) ;;
            *:*) fuzzed=$((fuzzed + 1)) ;;
            *) missed=$((missed + 1)) ;;
        esac
    done
    echo "at once $at_once, found by fuzzing $fuzzed, not found $missed, broken $broken"
} | tee "$logs/fuzz.txt"
