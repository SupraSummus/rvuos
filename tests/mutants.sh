#!/bin/sh
# Validate the self-check by planting bugs in the kernel.
# Each mutant is a sed expression applied to one kernel file in a scratch copy;
# replaying the corpus there, or a short fuzz run, must report an invariant violation.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mutant() {
    name=$1 file=$2 expr=$3
    rm -rf "$work/tree"
    mkdir -p "$work/tree"
    cp -r "$root/kernel" "$root/host" "$root/include" "$root/Makefile" "$root/tests" "$work/tree/"
    sed -i "$expr" "$work/tree/$file"
    if cmp -s "$root/$file" "$work/tree/$file"; then
        echo "mutant $name: sed expression did not change $file" >&2
        exit 1
    fi
    # The corpus replay should catch it; give a short fuzz run a chance otherwise.
    if (cd "$work/tree" && make -s host-test >"$work/log" 2>&1) &&
       (cd "$work/tree" && make -s fuzz FUZZ_TIME=60 >"$work/log" 2>&1); then
        echo "mutant $name: NOT caught" >&2
        exit 1
    fi
    grep -q 'invariant violated' "$work/log" || {
        echo "mutant $name: failed without an invariant report" >&2
        tail -5 "$work/log" >&2
        exit 1
    }
    echo "mutant $name: caught"
}

mutant no-pool-overlap-check kernel/process.c \
    's/if (pool_overlaps(base, size)) {/if (0) {/'
mutant copy-widens-rights kernel/syscall.c \
    's/src.rights &= (uint8_t)arg\[3\];/src.rights = (uint8_t)arg[3];/'
mutant pmp-extra-write kernel/process.c \
    's/PMP_A_TOR | rights_to_pmp(s->rights)/PMP_A_TOR | rights_to_pmp(s->rights) | PMP_W/'
mutant wake-keeps-waiting-on kernel/syscall.c \
    's/^    t->waiting_on = 0;$//'
# A destroy that does not sweep leaves capabilities naming dead objects,
# and they come back to life when the memory is rebuilt into the same type.
mutant destroy-without-sweep kernel/cap.c \
    's/if (o->type != CAP_CAPTABLE) {/if (o->type != CAP_CAPTABLE || 1) {/'
# A destroy that leaves a thread blocked on a notification it took away.
mutant destroy-keeps-waiters kernel/pool.c \
    's/        sched_unblock_range(p->base, p->size);//'
# A pool that forgets who created it hangs from nothing, not from the boot pool.
mutant pool-forgets-parent kernel/syscall.c \
    's/pool_create(base, size, cap->rights, obj_pool(&t->hdr))/pool_create(base, size, cap->rights, NULL)/'
# A destroy that spares the pools below leaves them naming a parent that is gone.
mutant destroy-spares-children kernel/pool.c \
    's/if (!pool_under(p, pool)) {/if (p != pool) {/'
# A carve that ignores the grain hands out a region hardware would round;
# the corpus has such carves and the 32-byte-grain build sees them.
mutant carve-ignores-grain kernel/syscall.c \
    's/if (!grain_aligned(off, len) || len == 0/if ((off | len) \& 3u || len == 0/'
