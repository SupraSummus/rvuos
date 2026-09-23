#!/bin/sh
# Validate the checks by planting bugs in the kernel.
#
# A mutant is a patch under tests/mutants/, applied to a scratch copy of the tree,
# with the invariant it breaks written above the diff; `git apply` skips that text.
# To plant one, edit the kernel, save `git diff` under a name, and take the edit back.
# The patch applies with one line of context, so edits near the mutated line
# do not stale it; a change to the line itself makes the mutant broken.
#
# Each mutant runs against every check of `make check`.
# host-test must catch it with an invariant report: that is the check on the corpus.
# What test and qemu-replay catch is reported, and skipped when QEMU is not installed.
# A mutant that does not apply or does not build is broken, not caught.
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

# Run one make target in a mutant's tree; the log goes under build/mutants/.
run() {
    (cd "$tree" && make -s "$2" REPLAY_JOBS=1 >"$logs/$1.$2.log" 2>&1)
}

# Run one mutant and leave its result, caught, missed or broken, in the work directory.
# Each message is one write, so parallel mutants do not interleave.
mutant() {
    name=$1
    tree=$work/tree-$name
    result=$work/result/$name
    patch=$root/tests/mutants/$name.patch
    cp -a "$work/base" "$tree"
    if ! (cd "$tree" && git apply -C1 "$patch") >"$logs/$name.apply.log" 2>&1; then
        printf 'mutant %s: does not apply\n%s\n' "$name" "$(cat "$logs/$name.apply.log")" >&2
        echo broken >"$result"
        return
    fi
    if ! run "$name" all; then
        printf 'mutant %s: does not build\n%s\n' "$name" "$(tail -5 "$logs/$name.all.log")" >&2
        echo broken >"$result"
        return
    fi

    if run "$name" host-test; then
        host=missed
    elif grep -q 'invariant violated' "$logs/$name.host-test.log"; then
        host=caught
    else
        printf 'mutant %s: host-test failed without an invariant report\n%s\n' \
            "$name" "$(tail -5 "$logs/$name.host-test.log")" >&2
        echo broken >"$result"
        return
    fi
    echo "$host" >"$result"

    if [ -n "$qemu" ]; then
        if run "$name" test; then qemu_test=missed; else qemu_test=caught; fi
        if run "$name" qemu-replay; then replay=missed; else replay=caught; fi
        echo "mutant $name: host-test $host, test $qemu_test, qemu-replay $replay"
    else
        echo "mutant $name: host-test $host"
    fi
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
[ -n "$qemu" ] || echo "qemu-system-riscv32 not found: test and qemu-replay are skipped" >&2

cp -r "$root/kernel" "$root/host" "$root/include" "$root/user" "$root/Makefile" "$root/tests" \
    "$work/base/"
if ! (cd "$work/base" && make -s -j"$jobs" all host-test) >"$logs/unmutated.log" 2>&1; then
    echo "the unmutated tree does not build or fails host-test, see build/mutants/unmutated.log" >&2
    exit 1
fi

export MUTANTS_WORK="$work" MUTANTS_QEMU="$qemu"
printf '%s\n' $names | xargs -P "$jobs" -I{} "$0" --mutant {}

broken=""
missed=""
for name in $names; do
    case $(cat "$work/result/$name" 2>/dev/null || echo broken) in
        caught) ;;
        missed) missed="$missed $name" ;;
        *) broken="$broken $name" ;;
    esac
done

status=0
if [ -n "$broken" ]; then
    echo "broken mutants:$broken" >&2
    status=1
fi
if [ -n "$missed" ]; then
    echo "mutants the host replay missed:$missed" >&2
    status=1
fi
exit $status
