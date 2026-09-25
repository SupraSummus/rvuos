#!/bin/sh
# Validate the checks by planting bugs in the kernel.
#
# A mutant is a patch under tests/mutants/, applied to a scratch copy of the tree,
# with the invariant it breaks written above the diff; `git apply` skips that text.
# To plant one, edit the kernel, save `git diff` under a name, and take the edit back.
# The patch applies with one line of context, so edits near the mutated line
# do not stale it; a change to the line itself makes the mutant broken.
# `make mutants-refresh` writes the patches again against the kernel as it is,
# carrying a mutant over a change of its lines' indentation too.
#
# Below the prose, the header says what the checks do on the mutant:
#
#   Caught-by: fuzz fuzz-pmp8 fuzz-grain32 fuzz-work qemu-replay
#   Report: the count of armed sources is wrong
#
# Caught-by is exactly the checks that catch it, in the order they run:
# stack-depth loop-bounds fuzz fuzz-pmp8 fuzz-grain32 fuzz-work test qemu-replay.
# Report is exactly the invariant reports the seeds give on the host harnesses,
# one per line with the numbers taken out;
# the corpus changes with every minimisation, so its reports are left out.
# A mutant whose header the checks disagree with fails, and the run prints what they say,
# which is where a new mutant takes its lines from.
#
# stack-depth and loop-bounds catch a mutant by refusing the link;
# a kernel they refuse still runs on the host harnesses, but not on QEMU.
# The host harnesses catch a mutant with an invariant report, and test and qemu-replay by failing.
# The link or a seed on a host harness must catch every mutant:
# the corpus alone would lose it at the next minimisation, and QEMU alone is not enough.
# Without QEMU, test and qemu-replay are left out of the header's list too.
# A mutant that does not apply or does not build is broken, not caught,
# and so is one that fails a host harness without an invariant report.
#
# The unmutated tree is built once and must pass host-test.
# Each mutant runs in a copy of it with the timestamps kept,
# so make recompiles only what the patch touches.
# The mutants run in parallel, one per processor or -j of them,
# and each replays on one QEMU.
#
# Usage: tests/mutants.sh [-j jobs] [name...]      logs go under build/mutants/

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
logs=$root/build/mutants
harnesses="fuzz fuzz-pmp8 fuzz-grain32 fuzz-work"

# Run one make target in a mutant's tree; the log goes under build/mutants/.
run() {
    (cd "$tree" && make -s "$1" REPLAY_JOBS=1 >"$logs/$name.$1.log" 2>&1)
}

# What the mutant's header lines of one name say, a line for each.
header() {
    sed -n "/^diff --git/q; s/^$1: *//p" "$patch"
}

broken() {
    printf 'mutant %s: %s\n%s\n' "$name" "$1" "$2" >&2
    echo broken >"$work/result/$name"
}

# Replay the seeds one by one on a host harness, noting their reports,
# then the corpus at once; the harness runs in the tree, where libFuzzer leaves what failed.
replay() {
    bin=$tree/build/host/$1
    hit=""
    for seed in "$tree"/tests/seeds/*; do
        (cd "$tree" && "$bin" --verbose "$seed") >"$out" 2>&1 && continue
        line=$(grep -o 'invariant violated: .*' "$out" | head -1 | sed 's/^invariant violated: //;
            s/ 0x[0-9a-f]\{8\}//g; s/:[0-9][0-9]*//g; s/\<[0-9][0-9]*\>/N/g')
        if [ -z "$line" ]; then
            broken "$1 fails seed ${seed##*/} without an invariant report" "$(tail -5 "$out")"
            return 1
        fi
        reports="$reports$line
"
        hit=yes
    done
    if ! (cd "$tree" && "$bin" -runs=0 tests/corpus) >"$logs/$name.$1.log" 2>&1; then
        if ! grep -q 'invariant violated' "$logs/$name.$1.log"; then
            broken "$1 fails the corpus without an invariant report" "$(tail -5 "$logs/$name.$1.log")"
            return 1
        fi
        hit=yes
    fi
    [ -z "$hit" ] || caught="$caught $1"
}

# Run one mutant and leave its result, caught, differs, missed or broken, in the work directory.
# Each message is one write, so parallel mutants do not interleave.
mutant() {
    name=$1
    tree=$work/tree-$name
    patch=$root/tests/mutants/$name.patch
    out=$work/out-$name
    cp -a "$work/base" "$tree"
    if ! (cd "$tree" && git apply -C1 "$patch") >"$out" 2>&1; then
        broken "does not apply" "$(cat "$out")"
        return
    fi
    caught=""
    reports=""
    linked=yes
    if ! run all; then
        linked=no
        for tool in stack-depth loop-bounds; do
            if grep -q "^$tool: " "$logs/$name.all.log"; then caught="$caught $tool"; fi
        done
        if [ -z "$caught" ]; then
            broken "does not build" "$(tail -5 "$logs/$name.all.log")"
            return
        fi
    fi
    if ! run host-harnesses; then
        broken "host harnesses do not build" "$(tail -5 "$logs/$name.host-harnesses.log")"
        return
    fi
    for h in $harnesses; do
        replay "$h" || return
    done
    if [ -n "$qemu" ] && [ $linked = yes ]; then
        run test || caught="$caught test"
        run qemu-replay || caught="$caught qemu-replay"
    fi
    caught=${caught# }
    reports=$(printf '%s' "$reports" | sort -u)

    want=$(header Caught-by)
    [ -n "$qemu" ] || want=$(echo "$want" | sed 's/ *\<test\>//; s/ *qemu-replay//; s/^ //')
    if [ -z "$reports" ] && [ "$linked" = yes ]; then
        result=missed
    elif [ "$want" = "$caught" ] && [ "$(header Report | sort -u)" = "$reports" ]; then
        echo caught >"$work/result/$name"
        echo "mutant $name: $caught"
        return
    else
        result=differs
    fi
    echo $result >"$work/result/$name"
    printf 'mutant %s: the header says\n%s\nand the checks say\n%s\n' "$name" \
        "$(echo "  Caught-by: $want"; header Report | sed 's/^/  Report: /')" \
        "$(echo "  Caught-by: $caught"; [ -z "$reports" ] || echo "$reports" | sed 's/^/  Report: /')"
}

# xargs calls the script back once per mutant, with the state in the environment.
if [ "${1-}" = --mutant ]; then
    work=$MUTANTS_WORK
    qemu=$MUTANTS_QEMU
    mutant "$2"
    rm -rf "$tree"
    exit 0
fi

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
if [ "${1-}" = -j ]; then
    jobs=$2
    shift 2
fi

[ $# -gt 0 ] || set -- "$root"/tests/mutants/*.patch
names=""
for arg in "$@"; do
    name=$(basename "$arg" .patch)
    if [ ! -f "$root/tests/mutants/$name.patch" ]; then
        echo "mutant $name: no such patch" >&2
        exit 1
    fi
    names="$names $name"
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$logs" "$work/base" "$work/result"

qemu=$(command -v qemu-system-riscv32 || true)
[ -n "$qemu" ] || echo "qemu-system-riscv32 not found: test and qemu-replay are left out" >&2

cp -r "$root/kernel" "$root/host" "$root/include" "$root/user" "$root/Makefile" "$root/tests" \
    "$root/tools" "$root/DESIGN.md" "$work/base/"
if ! (cd "$work/base" && make -s -j"$jobs" all host-test) >"$logs/unmutated.log" 2>&1; then
    echo "the unmutated tree does not build or fails host-test, see build/mutants/unmutated.log" >&2
    exit 1
fi

export MUTANTS_WORK="$work" MUTANTS_QEMU="$qemu"
printf '%s\n' $names | xargs -P "$jobs" -I{} "$0" --mutant {}

broken=""
missed=""
differs=""
for name in $names; do
    case $(cat "$work/result/$name" 2>/dev/null || echo broken) in
        caught) ;;
        missed) missed="$missed $name" ;;
        differs) differs="$differs $name" ;;
        *) broken="$broken $name" ;;
    esac
done

status=0
if [ -n "$broken" ]; then
    echo "broken mutants:$broken" >&2
    status=1
fi
if [ -n "$missed" ]; then
    echo "mutants neither the link nor a seed catches:$missed" >&2
    status=1
fi
if [ -n "$differs" ]; then
    echo "mutants whose header the checks disagree with:$differs" >&2
    status=1
fi
exit $status
