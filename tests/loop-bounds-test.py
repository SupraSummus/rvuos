#!/usr/bin/env python3
"""tools/loop-bounds.py's jump tables on hand-made functions:
the one it follows, and each way around its bound check, which it must not follow.
No kernel clang builds has these shapes, so nothing else would show a hole reopened."""

import importlib.util
import re
import struct
import sys

sys.path.insert(0, "tools")
spec = importlib.util.spec_from_file_location("loop_bounds", "tools/loop-bounds.py")
lb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lb)

# A switch on a0 below 3, its shift hoisted above the check, its table at 0x1000.
# 0x24 is the one word of data pointing into the function, where a jr not followed goes too.
PLAIN = {
    0x00: "li a1, 0x3",
    0x04: "slli a2, a0, 0x2",
    0x08: "bgeu a0, a1, @2c",
    0x0c: "nop",
    0x10: "lui a3, 0x1",
    0x14: "addi a3, a3, 0x0",
    0x18: "add a4, a2, a3",
    0x1c: "lw a5, 0x0(a4)",
    0x20: "jr a5",
    0x24: "li a2, 0x400",
    0x28: "j @2c",
    0x2c: "ret",
}
CASES = [  # what, the lines it changes, the table's target, whether the table is followed
    ("the plain switch", {}, 0x2c, True),
    ("a jump between the check and the table", {0x0c: "j @2c"}, 0x2c, False),
    ("another way into the table's block", {0x28: "j @10"}, 0x2c, False),
    ("a check that branches into the table", {0x08: "bgeu a0, a1, @0c"}, 0x2c, False),
    ("a call between the check and the table", {0x0c: "jal @40"}, 0x2c, False),
    # The way back runs through the jr's own edge, which no word of data gives.
    ("an index written after the check, the table leading back to it",
     {0x0c: "addi a0, a0, 0x1", 0x24: "slli a2, a0, 0x2", 0x28: "j @08"}, 0x28, False),
]

failures = 0
for what, change, target, follows in CASES:
    insns = []
    for pc, line in sorted({**PLAIN, **change}.items()):
        mnem, _, ops = line.partition(" ")
        insns.append((pc, mnem, re.sub(r"@(\w+)", r"0x\1 <f+0x\1>", ops)))
    image = struct.pack("<3I", target, target, target)
    sections = [(".rodata", 1, lb.SHF_ALLOC, 0x1000, 0, len(image), 0, 0)]
    blocks = lb.blocks_of(0, 0x30, insns, {0: {0x24}}, {0x40}, image, sections)
    jr = max(b for b in blocks if b <= 0x20)
    if (blocks[jr].succ == {target}) != follows:
        failures += 1
        print(f"loop-bounds-test: FAIL {what}: the table is {'not ' * follows}followed")
if failures:
    sys.exit(f"loop-bounds-test: {failures} failures")
print(f"loop-bounds-test: ok, {len(CASES)} tests")
