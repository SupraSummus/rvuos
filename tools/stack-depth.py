#!/usr/bin/env python3
"""Bound the kernel's stack from its image and fail when it may overflow.

The bound is the heaviest call chain, each function weighing its frame;
DESIGN.md, "Bounded stack", says why that is the whole stack.
The frames come from clang's -fstack-usage, and one of run-time size fails.
A function with no .su entry is assembly,
weighing what its own instructions take off sp.
The calls come from the disassembly.
A jal or a jump to another function's first instruction is a call,
a jump into the middle of one fails,
and a jump back to a function's own start is a loop.
A jalr or jr through a register may reach any C function whose address is taken,
formed by an lui or auipc pair or stored in the kernel's data,
so it counts as a call to each; that covers jump tables too, too many never too few.
A C function nothing calls fails,
since only assembly is entered by the hardware:
the function is dead, or reached in a way this tool does not see.

Usage: stack-depth.py [--objdump tool] kernel.elf file.su...
"""

import argparse
import sys

from kimage import Failure, load


def heaviest(graph, entries):
    """The heaviest chain from an entry, as its weight and its names."""
    depth = {}
    via = {}
    active = []

    def visit(addr):
        if addr in active:
            cycle = active[active.index(addr):] + [addr]
            raise Failure("the kernel recurses: " + " > ".join(graph[a].name for a in cycle))
        if addr in depth:
            return
        active.append(addr)
        depth[addr], via[addr] = graph[addr].frame, None
        for callee in sorted(graph[addr].calls):
            visit(callee)
            if graph[addr].frame + depth[callee] > depth[addr]:
                depth[addr], via[addr] = graph[addr].frame + depth[callee], callee
        active.pop()

    for addr in graph:
        visit(addr)
    top = max(entries, key=lambda a: depth[a])
    names = []
    a = top
    while a is not None:
        names.append(f"{graph[a].name} {graph[a].frame}")
        a = via[a]
    return depth[top], " > ".join(names)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("elf")
    ap.add_argument("su", nargs="+")
    args = ap.parse_args()

    try:
        _, _, symbols, frames, _, graph, _ = load(args.objdump, args.elf, args.su)
        size = symbols["__kernel_stack_top"] - symbols["__kernel_stack_bottom"]

        called = set().union(*(fn.calls for fn in graph.values()))
        entries = [a for a in graph if a not in called]
        for a in entries:
            if graph[a].name in frames:
                raise Failure(f"nothing calls {graph[a].name}; "
                              "it is dead code, or called in a way this tool does not see")

        worst, route = heaviest(graph, entries)
        if worst > size:
            raise Failure(f"the kernel may take {worst} bytes of stack, "
                          f"and KERNEL_STACK_SIZE in kernel/kernel.ld.S gives {size}: {route}")
        print(f"kernel stack: at most {worst} of {size} bytes, through {route}")
        return 0
    except Failure as e:
        print(f"stack-depth: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
