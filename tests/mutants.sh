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
# Usage: tests/mutants.sh [name...]      logs go under build/mutants/

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
logs=$root/build/mutants
mkdir -p "$logs"

qemu=$(command -v qemu-system-riscv32 || true)
[ -n "$qemu" ] || echo "qemu-system-riscv32 not found: test and qemu-replay are skipped" >&2

broken=""
missed=""

# Run one make target in the scratch tree; the log goes under build/mutants/.
run() {
    (cd "$work/tree" && make -s "$2" >"$logs/$1.$2.log" 2>&1)
}

mutant() {
    name=$1
    patch=$root/tests/mutants/$name.patch
    if [ ! -f "$patch" ]; then
        echo "mutant $name: no such patch" >&2
        exit 1
    fi
    rm -rf "$work/tree"
    mkdir -p "$work/tree"
    cp -r "$root/kernel" "$root/host" "$root/include" "$root/user" "$root/Makefile" "$root/tests" \
        "$work/tree/"
    if ! (cd "$work/tree" && git apply -C1 "$patch") >"$logs/$name.apply.log" 2>&1; then
        echo "mutant $name: does not apply" >&2
        cat "$logs/$name.apply.log" >&2
        broken="$broken $name"
        return
    fi
    if ! run "$name" all; then
        echo "mutant $name: does not build" >&2
        tail -5 "$logs/$name.all.log" >&2
        broken="$broken $name"
        return
    fi

    if run "$name" host-test; then
        host=missed
        missed="$missed $name"
    elif grep -q 'invariant violated' "$logs/$name.host-test.log"; then
        host=caught
    else
        echo "mutant $name: host-test failed without an invariant report" >&2
        tail -5 "$logs/$name.host-test.log" >&2
        broken="$broken $name"
        return
    fi

    if [ -n "$qemu" ]; then
        if run "$name" test; then qemu_test=missed; else qemu_test=caught; fi
        if run "$name" qemu-replay; then replay=missed; else replay=caught; fi
        echo "mutant $name: host-test $host, test $qemu_test, qemu-replay $replay"
    else
        echo "mutant $name: host-test $host"
    fi
}

[ $# -gt 0 ] || set -- "$root"/tests/mutants/*.patch
for arg in "$@"; do
    mutant "$(basename "$arg" .patch)"
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
